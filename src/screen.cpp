/*
 * $Id: screen.cpp 3071 2014-07-26 18:01:08Z darren_janeczek $
 */

#include <cstdio>
#include <cstdarg>
#include <cfloat>
#include <cstring>
#include "u4.h"

#include "screen.h"

#include "config.h"
#include "context.h"
#include "dungeonview.h"
#include "error.h"
#include "event.h"
#include "intro.h"
#include "imagemgr.h"
#include "location.h"
#include "names.h"
#include "object.h"
#include "player.h"
#include "savegame.h"
#include "settings.h"
#include "textcolor.h"
#include "tileanim.h"
#include "tileset.h"
#include "tileview.h"
#include "annotation.h"

enum LayoutType {
    LAYOUT_STANDARD,
    LAYOUT_GEM,
    LAYOUT_DUNGEONGEM
};

struct Layout {
    string name;
    LayoutType type;
    struct {
        int width, height;
    } tileshape;
    struct {
        int x, y;
        int width, height;
    } viewport;
};



using std::vector;

void screenLoadGraphicsFromConf(void);
Layout *screenLoadLayoutFromConf(const ConfigElement &conf);
void screenShowGemTile(Layout *layout, Map *map, MapTile &t, bool focus, int x, int y);

vector<Layout *> layouts;
vector<TileAnimSet *> tileanimSets;
vector<string> gemLayoutNames;
vector<string> filterNames;
vector<string> lineOfSightStyles;
Layout *gemlayout = NULL;
std::map<string, int> dungeonTileChars;
TileAnimSet *tileanims = NULL;
ImageInfo *charsetInfo = NULL;
ImageInfo *gemTilesInfo = NULL;

void screenFindLineOfSight(vector<MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]);
void screenFindLineOfSightDOS(vector<MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]);
void screenFindLineOfSightEnhanced(vector<MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]);

int screenNeedPrompt = 1;
int screenCurrentCycle = 0;
int screenCursorX = 0;
int screenCursorY = 0;
int screenCursorStatus = 0;
int screenCursorEnabled = 1;
int screenLos[VIEWPORT_W][VIEWPORT_H];

static const int BufferSize = 1024;

extern bool verbose;

// Just extern the system functions here. That way people aren't tempted to call them as part of the public API.
extern void screenInit_sys();
extern void screenDelete_sys();

void screenInit() {
    filterNames.clear();
    filterNames.push_back("point");
    
    lineOfSightStyles.clear();
    lineOfSightStyles.push_back("DOS");
    lineOfSightStyles.push_back("Enhanced");
    
    charsetInfo = NULL;    
    gemTilesInfo = NULL;
    
    screenLoadGraphicsFromConf();
      
    screenInit_sys();
    
    /* if we can't use vga, reset to default:ega */
    if (!u4isUpgradeAvailable() && settings.videoType == 1)
        settings.videoType = 0;
    
    
    KeyHandler::setKeyRepeat(settings.keydelay, settings.keyinterval);
    
    /* find the tile animations for our tileset */
    tileanims = NULL;
    for (std::vector<TileAnimSet *>::const_iterator i = tileanimSets.begin(); i != tileanimSets.end(); i++) {
        TileAnimSet *set = *i;
        if (set->name == "EGA" && !settings.videoType)
            tileanims = set;
        else if (set->name == "VGA" && settings.videoType)
            tileanims = set;
    }
    if (!tileanims)
        xu4_error(XU4_LOG_ERR, "unable to find tile animations for \"%s\" video mode in graphics.xml", settings.videoType ? "VGA" : "EGA");
    
    dungeonTileChars.clear();
    dungeonTileChars["brick_floor"] = CHARSET_FLOOR;
    dungeonTileChars["up_ladder"] = CHARSET_LADDER_UP;
    dungeonTileChars["down_ladder"] = CHARSET_LADDER_DOWN;      
    dungeonTileChars["up_down_ladder"] = CHARSET_LADDER_UPDOWN;
    dungeonTileChars["chest"] = '$';
    dungeonTileChars["ceiling_hole"] = CHARSET_FLOOR;
    dungeonTileChars["floor_hole"] = CHARSET_FLOOR;
    dungeonTileChars["magic_orb"] = CHARSET_ORB;
    dungeonTileChars["ceiling_hole"] = 'T';
    dungeonTileChars["floor_hole"] = 'T';
    dungeonTileChars["fountain"] = 'F';
    dungeonTileChars["secret_door"] = CHARSET_SDOOR;
    dungeonTileChars["brick_wall"] = CHARSET_WALL;
    dungeonTileChars["dungeon_door"] = CHARSET_ROOM;
    dungeonTileChars["avatar"] = CHARSET_REDDOT;
    dungeonTileChars["dungeon_room"] = CHARSET_ROOM;
    dungeonTileChars["dungeon_altar"] = CHARSET_ANKH;
    dungeonTileChars["energy_field"] = '^';
    dungeonTileChars["fire_field"] = '^';
    dungeonTileChars["poison_field"] = '^';
    dungeonTileChars["sleep_field"] = '^';
}

void screenDelete() {
    std::vector<Layout *>::const_iterator i;
    for (i = layouts.begin(); i != layouts.end(); i++)
        delete(*i);
    layouts.clear();
    screenDelete_sys();
    
    ImageMgr::destroy();
}


/**
 * Re-initializes the screen and implements any changes made in settings
 */
void screenReInit() {        
    intro->deleteIntro();       /* delete intro stuff */
    Tileset::unloadAllImages(); /* unload tilesets, which will be reloaded lazily as needed */
    ImageMgr::destroy();
    tileanims = NULL;
    screenDelete(); /* delete screen stuff */
    screenInit();   /* re-init screen stuff (loading new backgrounds, etc.) */
    intro->init();    /* re-fix the backgrounds loaded and scale images, etc. */
}

void screenTextAt(int x, int y, const char *fmt, ...) {
    char buffer[BufferSize];
    unsigned int i;

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, BufferSize, fmt, args);
    va_end(args);

    for (i = 0; i < strlen(buffer); i++)
        screenShowChar(buffer[i], x + i, y);
}

void screenPrompt() {
    if (screenNeedPrompt && screenCursorEnabled && c->col == 0) {
        screenMessage("%c", CHARSET_PROMPT);
        screenNeedPrompt = 0;
    }
}

void screenMessage(const char *fmt, ...) {
    if (!c)
    	return; //Because some cases (like the intro) don't have the context initiated.
    char buffer[BufferSize];
    unsigned int i;
    int wordlen;    

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, BufferSize, fmt, args);
    va_end(args);

    screenHideCursor();

    /* scroll the message area, if necessary */
    if (c->line == 12) {
        screenScrollMessageArea();
        c->line--;
    }

    for (i = 0; i < strlen(buffer); i++) {
        // include whitespace and color-change codes
        wordlen = strcspn(buffer + i, " \b\t\n\024\025\026\027\030\031");

        /* backspace */
        if (buffer[i] == '\b') {
            c->col--;
            if (c->col < 0) {
                c->col += 16;
                c->line--;
            }
            continue;
        }

		/* color-change codes */
		switch (buffer[i])
		{
			case FG_GREY:
			case FG_BLUE:
            case FG_PURPLE:
			case FG_GREEN:
			case FG_RED:
			case FG_YELLOW:
			case FG_WHITE:
				screenTextColor(buffer[i]);
				continue;
		}

        /* check for word wrap */
        if ((c->col + wordlen > 16) || buffer[i] == '\n' || c->col == 16) {
            if (buffer[i] == '\n' || buffer[i] == ' ')
                i++;
            c->line++;
            c->col = 0;
            screenMessage("%s", buffer + i);
            return;
        }

        /* code for move cursor right */
        if (buffer[i] == 0x12) {
            c->col++;
            continue;
        }
        /* don't show a space in column 1.  Helps with Hawkwind. */
        if (buffer[i] == ' ' && c->col == 0)
          continue; 
        screenShowChar(buffer[i], TEXT_AREA_X + c->col, TEXT_AREA_Y + c->line);
        c->col++;
    }

    screenSetCursorPos(TEXT_AREA_X + c->col, TEXT_AREA_Y + c->line);
    screenShowCursor();

    screenNeedPrompt = 1;
}

const vector<string> &screenGetFilterNames() {
    return filterNames;
}

const vector<string> &screenGetGemLayoutNames() {
    return gemLayoutNames;
}

const vector<string> &screenGetLineOfSightStyles() {
    return lineOfSightStyles;
}

void screenLoadGraphicsFromConf() {
    const Config *config = Config::getInstance();
    
    vector<ConfigElement> graphicsConf = config->getElement("graphics").getChildren();
    for (std::vector<ConfigElement>::iterator conf = graphicsConf.begin(); conf != graphicsConf.end(); conf++) {
        
        if (conf->getName() == "layout")
            layouts.push_back(screenLoadLayoutFromConf(*conf));
        else if (conf->getName() == "tileanimset")
            tileanimSets.push_back(new TileAnimSet(*conf));
    }
    
    gemLayoutNames.clear();
    std::vector<Layout *>::const_iterator i;
    for (i = layouts.begin(); i != layouts.end(); i++) {
        Layout *layout = *i;
        if (layout->type == LAYOUT_GEM) {
            gemLayoutNames.push_back(layout->name);
        }
    }
    
    /*
     * Find gem layout to use.
     */
    for (i = layouts.begin(); i != layouts.end(); i++) {
        Layout *layout = *i;
        
        if (layout->type == LAYOUT_GEM && layout->name == "Standard" && !settings.gemLayout) {
            gemlayout = layout;
            break;
        }
        else if (layout->type == LAYOUT_GEM && layout->name == "Full Viewport" && settings.gemLayout) {
            gemlayout = layout;
            break;
        }
    }
    if (!gemlayout)
        xu4_error(XU4_LOG_ERR, "no gem layout named %s found!\n", settings.gemLayout ? "Full Viewport" : "Standard");
}

Layout *screenLoadLayoutFromConf(const ConfigElement &conf) {
    Layout *layout;
    static const char *typeEnumStrings[] = { "standard", "gem", "dungeon_gem", NULL };
    
    layout = new Layout;
    layout->name = conf.getString("name");
    layout->type = static_cast<LayoutType>(conf.getEnum("type", typeEnumStrings));
    
    vector<ConfigElement> children = conf.getChildren();
    for (std::vector<ConfigElement>::iterator i = children.begin(); i != children.end(); i++) {
        if (i->getName() == "tileshape") {
            layout->tileshape.width = i->getInt("width");
            layout->tileshape.height = i->getInt("height");
        }
        else if (i->getName() == "viewport") {
            layout->viewport.x = i->getInt("x");
            layout->viewport.y = i->getInt("y");
            layout->viewport.width = i->getInt("width");
            layout->viewport.height = i->getInt("height");
        }
    }
    
    return layout;
}



vector<MapTile> screenViewportTile(unsigned int width, unsigned int height, int x, int y, bool &focus) {
    Coords center = c->location->coords;    
    static MapTile grass = c->location->map->tileset->getByName("grass")->getId();
    
    if (c->location->map->width <= width &&
        c->location->map->height <= height) {
        center.x = c->location->map->width / 2;
        center.y = c->location->map->height / 2;
    }

    Coords tc = center;

    tc.x += x - (width / 2);
    tc.y += y - (height / 2);

    /* Wrap the location if we can */    
    wrap(&tc, c->location->map);

    /* off the edge of the map: pad with grass tiles */
    if (MAP_IS_OOB(c->location->map, tc)) {        
        focus = false;
        vector<MapTile> result;
        result.push_back(grass);
        return result;
    }
    
    Coords tc2(tc);

    return c->location->tilesAt(tc2, focus);
}

bool screenTileUpdate(TileView *view, const Coords &coords, bool redraw)
{
	if (c->location->map->flags & FIRST_PERSON)
		return false;

	// Get the tiles
	bool focus;
	Coords mc;
	mc.x = coords.x; mc.y = coords.y; mc.z = coords.z;
	wrap(&mc, c->location->map);
	Coords mc2(mc);
	vector<MapTile> tiles = c->location->tilesAt(mc2, focus);

	// Get the screen coordinates
	int x = coords.x;
	int y = coords.y;

	if (c->location->map->width > VIEWPORT_W || c->location->map->height > VIEWPORT_H)
	{
		//Center the coordinates to the viewport if you're on centered-view map.
		x = x - c->location->coords.x + VIEWPORT_W / 2;
		y = y - c->location->coords.y + VIEWPORT_H / 2;
	}

	// Draw if it is on screen
	if (x >= 0 && y >= 0 && x < VIEWPORT_W && y < VIEWPORT_H && screenLos[x][y])
	{
		view->drawTile(tiles, focus, x, y);

		if (redraw)
		{
			screenRedrawMapArea();
			return true;
		}
		//return true;
	}
	return false;
}

/**
 * Redraw the screen.  If showmap is set, the normal map is drawn in
 * the map area.  If blackout is set, the map area is blacked out. If
 * neither is set, the map area is left untouched.
 */
void screenUpdate(TileView *view, bool showmap, bool blackout) {
    xu4_assert(c != NULL, "context has not yet been initialized");

    //screenLock();

    if (blackout)
    {
    	screenEraseMapArea();
    }
    else if (c->location->map->flags & FIRST_PERSON) {
    	DungeonViewer.display(c, view);
        screenRedrawMapArea();
    }

    else if (showmap) {
        static MapTile black = c->location->map->tileset->getByName("black")->getId();
        //static MapTile avatar = c->location->map->tileset->getByName("avatar")->getId();

        int x, y;

        vector<MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H];
        bool viewportFocus[VIEWPORT_W][VIEWPORT_H];

        for (y = 0; y < VIEWPORT_H; y++) {
            for (x = 0; x < VIEWPORT_W; x++) {
                viewportTiles[x][y] = screenViewportTile(VIEWPORT_W, VIEWPORT_H, x, y, viewportFocus[x][y]);
            }
        }

		screenFindLineOfSight(viewportTiles);

        for (y = 0; y < VIEWPORT_H; y++) {
            for (x = 0; x < VIEWPORT_W; x++) {
                if (screenLos[x][y]) {
               		view->drawTile(viewportTiles[x][y], viewportFocus[x][y], x, y);
                }
                else
                    view->drawTile(black, false, x, y);
            }
        }
        screenRedrawMapArea();
    }

    screenUpdateCursor();
    screenUpdateMoons();
    screenUpdateWind();

    //screenUnlock();
}

/**
 * Draw an image or subimage on the screen.
 */
void screenDrawImage(const string &name, int x, int y) {
    ImageInfo *info = imageMgr->get(name);
    if (info) {
    	info->image->alphaOn();
        info->image->draw(x, y);
        return;
    }
    
    SubImage *subimage = imageMgr->getSubImage(name);
    if (subimage) {
        info = imageMgr->get(subimage->srcImageName);
    }
    
	if (info) {
		info->image->alphaOn();
        
        if (info) {
            info->image->drawSubRect(x, y,
                                     subimage->x * (settings.scale / info->prescale),
                                     subimage->y * (settings.scale / info->prescale),
                                     subimage->width * (settings.scale / info->prescale),
                                     subimage->height * (settings.scale / info->prescale));
            return;
        }
    }
    xu4_error(XU4_LOG_ERR, "ERROR 1006: Unable to load the image \"%s\".\t\n\nIs Ultima IV installed?\n\nVisit the XU4 website for additional information.\n\thttp://xu4.sourceforge.net/", name.c_str());
}

void screenDrawImageInMapArea(const string &name) {
    ImageInfo *info;
    
    info = imageMgr->get(name);
    if (!info)
        xu4_error(XU4_LOG_ERR, "ERROR 1004: Unable to load data files.\t\n\nIs Ultima IV installed?\n\nVisit the XU4 website for additional information.\n\thttp://xu4.sourceforge.net/");
    
    info->image->drawSubRect(BORDER_WIDTH * settings.scale, BORDER_HEIGHT * settings.scale,
                             BORDER_WIDTH * settings.scale, BORDER_HEIGHT * settings.scale,
                             VIEWPORT_W * TILE_WIDTH * settings.scale, 
                             VIEWPORT_H * TILE_HEIGHT * settings.scale);
}


/**
 * Change the current text color
 */
void screenTextColor(int color) {
    /*if (charsetInfo == NULL) {
        charsetInfo = imageMgr->get(BKGD_CHARSET);
        if (!charsetInfo)
            xu4_error(XU4_LOG_ERR, "ERROR 1003: Unable to load the \"%s\" data file.\t\n\nIs Ultima IV installed?\n\nVisit the XU4 website for additional information.\n\thttp://xu4.sourceforge.net/", BKGD_CHARSET);
    }
    
	if (!settings.enhancements || !settings.enhancementsOptions.textColorization) {
		return;
	}
    
	switch (color)
	{
		case FG_GREY:
		case FG_BLUE:
        case FG_PURPLE:
		case FG_GREEN:
		case FG_RED:
		case FG_YELLOW:
		case FG_WHITE:
			charsetInfo->image->setFontColorFG((ColorFG)color);
	}*/
}

/**
 * Draw a character from the charset onto the screen.
 */
void screenShowChar(int chr, int x, int y) {
    if (charsetInfo == NULL) {
        charsetInfo = imageMgr->get(BKGD_CHARSET);
        if (!charsetInfo)
            xu4_error(XU4_LOG_ERR, "ERROR 1001: Unable to load the \"%s\" data file.\t\n\nIs Ultima IV installed?\n\nVisit the XU4 website for additional information.\n\thttp://xu4.sourceforge.net/", BKGD_CHARSET);
    }
    
    charsetInfo->image->drawSubRect(x * charsetInfo->image->w, y * (CHAR_HEIGHT * settings.scale),
                                    0, chr * (CHAR_HEIGHT * settings.scale),
                                    charsetInfo->image->w, CHAR_HEIGHT * settings.scale);
}

/**
 * Scroll the text in the message area up one position.
 */
void screenScrollMessageArea() {
    xu4_assert(charsetInfo != NULL && charsetInfo->image != NULL, "charset not initialized!");
    
    Image *screen = imageMgr->get("screen")->image;
    
    screen->drawSubRectOn(screen, 
                          TEXT_AREA_X * charsetInfo->image->w, 
                          TEXT_AREA_Y * CHAR_HEIGHT * settings.scale,
                          TEXT_AREA_X * charsetInfo->image->w,
                          (TEXT_AREA_Y + 1) * CHAR_HEIGHT * settings.scale,
                          TEXT_AREA_W * charsetInfo->image->w,
                          (TEXT_AREA_H - 1) * CHAR_HEIGHT * settings.scale);
    
    
    screen->fillRect(TEXT_AREA_X * charsetInfo->image->w,
                     TEXT_AREA_Y * CHAR_HEIGHT * settings.scale + (TEXT_AREA_H - 1) * CHAR_HEIGHT * settings.scale,
                     TEXT_AREA_W * charsetInfo->image->w,
                     CHAR_HEIGHT * settings.scale,
                     0, 0, 0, 255);
    
    screenRedrawScreen();
}

void screenCycle() {
    if (++screenCurrentCycle >= SCR_CYCLE_MAX)
        screenCurrentCycle = 0;
    screenRedrawScreen();
}

void screenUpdateCursor() {
    int phase = screenCurrentCycle * SCR_CYCLE_PER_SECOND / SCR_CYCLE_MAX;

    xu4_assert(phase >= 0 && phase < 4, "derived an invalid cursor phase: %d", phase);

    if (screenCursorStatus) {
        screenShowChar(31 - phase, screenCursorX, screenCursorY);
        screenRedrawTextArea(screenCursorX, screenCursorY, 1, 1);
    }
}

void screenUpdateMoons() {
    int trammelChar, feluccaChar;

    /* show "L?" for the dungeon level */
    if (c->location->context == CTX_DUNGEON) {
        screenShowChar('L', 11, 0);
        screenShowChar('1'+c->location->coords.z, 12, 0);        
    }
    /* show the current moons (non-combat) */
    else if ((c->location->context & CTX_NON_COMBAT) == c->location->context) {
        trammelChar = (c->saveGame->trammelphase == 0) ?
            MOON_CHAR + 7 :
            MOON_CHAR + c->saveGame->trammelphase - 1;
        feluccaChar = (c->saveGame->feluccaphase == 0) ?
            MOON_CHAR + 7 :
            MOON_CHAR + c->saveGame->feluccaphase - 1;

        screenShowChar(trammelChar, 11, 0);
        screenShowChar(feluccaChar, 12, 0);        
    }

    screenRedrawTextArea(11, 0, 2, 1);
}

void screenUpdateWind() {   
    
    /* show the direction we're facing in the dungeon */
    if (c->location->context == CTX_DUNGEON) {
        screenEraseTextArea(WIND_AREA_X, WIND_AREA_Y, WIND_AREA_W, WIND_AREA_H);
        screenTextAt(WIND_AREA_X, WIND_AREA_Y, "Dir: %5s", getDirectionName((Direction)c->saveGame->orientation));        
    }
    /* show the wind direction */
    else if ((c->location->context & CTX_NON_COMBAT) == c->location->context) {
        screenEraseTextArea(WIND_AREA_X, WIND_AREA_Y, WIND_AREA_W, WIND_AREA_H);
        screenTextAt(WIND_AREA_X, WIND_AREA_Y, "Wind %5s", getDirectionName((Direction) c->windDirection));
    }
    screenRedrawTextArea(WIND_AREA_X, WIND_AREA_Y, WIND_AREA_W, WIND_AREA_H);
}

void screenShowCursor() {
    if (!screenCursorStatus && screenCursorEnabled) {
        screenCursorStatus = 1;
        screenUpdateCursor();
    }
}

void screenHideCursor() {
    if (screenCursorStatus) {
        screenEraseTextArea(screenCursorX, screenCursorY, 1, 1);
        screenRedrawTextArea(screenCursorX, screenCursorY, 1, 1);
    }
    screenCursorStatus = 0;
}

void screenEnableCursor(void) {
    screenCursorEnabled = 1;
}

void screenDisableCursor(void) {
    screenHideCursor();
    screenCursorEnabled = 0;
}

void screenSetCursorPos(int x, int y) {
    screenCursorX = x;
    screenCursorY = y;
}

/**
 * Finds which tiles in the viewport are visible from the avatars
 * location in the middle. (original DOS algorithm)
 */
void screenFindLineOfSight(vector <MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]) {
    int x, y;

    if (!c)
        return;

    /*
     * if the map has the no line of sight flag, all is visible
     */
    if (c->location->map->flags & NO_LINE_OF_SIGHT) {
        for (y = 0; y < VIEWPORT_H; y++) {
            for (x = 0; x < VIEWPORT_W; x++) {
                screenLos[x][y] = 1;
            }
        }
        return;
    }

    /*
     * otherwise calculate it from the map data
     */
    for (y = 0; y < VIEWPORT_H; y++) {
        for (x = 0; x < VIEWPORT_W; x++) {
            screenLos[x][y] = 0;
        }
    }

    if (!settings.lineOfSight)
        screenFindLineOfSightDOS(viewportTiles);
    else if (settings.lineOfSight)
        screenFindLineOfSightEnhanced(viewportTiles);
    else
        xu4_error(XU4_LOG_ERR, "unknown line of sight style %s!\n", settings.lineOfSight ? "Enhanced" : "DOS");
}        


/**
 * Finds which tiles in the viewport are visible from the avatars
 * location in the middle. (original DOS algorithm)
 */
void screenFindLineOfSightDOS(vector <MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]) {
    int x, y;

    screenLos[VIEWPORT_W / 2][VIEWPORT_H / 2] = 1;

    for (x = VIEWPORT_W / 2 - 1; x >= 0; x--)
        if (screenLos[x + 1][VIEWPORT_H / 2] &&
            !viewportTiles[x + 1][VIEWPORT_H / 2].front().getTileType()->isOpaque())
            screenLos[x][VIEWPORT_H / 2] = 1;

    for (x = VIEWPORT_W / 2 + 1; x < VIEWPORT_W; x++)
        if (screenLos[x - 1][VIEWPORT_H / 2] &&
            !viewportTiles[x - 1][VIEWPORT_H / 2].front().getTileType()->isOpaque())
            screenLos[x][VIEWPORT_H / 2] = 1;

    for (y = VIEWPORT_H / 2 - 1; y >= 0; y--)
        if (screenLos[VIEWPORT_W / 2][y + 1] &&
            !viewportTiles[VIEWPORT_W / 2][y + 1].front().getTileType()->isOpaque())
            screenLos[VIEWPORT_W / 2][y] = 1;

    for (y = VIEWPORT_H / 2 + 1; y < VIEWPORT_H; y++)
        if (screenLos[VIEWPORT_W / 2][y - 1] &&
            !viewportTiles[VIEWPORT_W / 2][y - 1].front().getTileType()->isOpaque())
            screenLos[VIEWPORT_W / 2][y] = 1;

    for (y = VIEWPORT_H / 2 - 1; y >= 0; y--) {
        
        for (x = VIEWPORT_W / 2 - 1; x >= 0; x--) {
            if (screenLos[x][y + 1] &&
                !viewportTiles[x][y + 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x + 1][y] &&
                     !viewportTiles[x + 1][y].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x + 1][y + 1] &&
                     !viewportTiles[x + 1][y + 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
        }
                
        for (x = VIEWPORT_W / 2 + 1; x < VIEWPORT_W; x++) {
            if (screenLos[x][y + 1] &&
                !viewportTiles[x][y + 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x - 1][y] &&
                     !viewportTiles[x - 1][y].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x - 1][y + 1] &&
                     !viewportTiles[x - 1][y + 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
        }
    }

    for (y = VIEWPORT_H / 2 + 1; y < VIEWPORT_H; y++) {
        
        for (x = VIEWPORT_W / 2 - 1; x >= 0; x--) {
            if (screenLos[x][y - 1] &&
                !viewportTiles[x][y - 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x + 1][y] &&
                     !viewportTiles[x + 1][y].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x + 1][y - 1] &&
                     !viewportTiles[x + 1][y - 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
        }
                
        for (x = VIEWPORT_W / 2 + 1; x < VIEWPORT_W; x++) {
            if (screenLos[x][y - 1] &&
                !viewportTiles[x][y - 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x - 1][y] &&
                     !viewportTiles[x - 1][y].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
            else if (screenLos[x - 1][y - 1] &&
                     !viewportTiles[x - 1][y - 1].front().getTileType()->isOpaque())
                screenLos[x][y] = 1;
        }
    }
}

/**
 * Finds which tiles in the viewport are visible from the avatars
 * location in the middle.
 *
 * A new, more accurate LOS function
 *
 * Based somewhat off Andy McFadden's 1994 article,
 *   "Improvements to a Fast Algorithm for Calculating Shading
 *   and Visibility in a Two-Dimensional Field"
 *   -----
 *   http://www.fadden.com/techmisc/fast-los.html
 *
 * This function uses a lookup table to get the correct shadowmap,
 * therefore, the table will need to be updated if the viewport
 * dimensions increase. Also, the function assumes that the
 * viewport width and height are odd values and that the player
 * is always at the center of the screen.
 */
void screenFindLineOfSightEnhanced(vector <MapTile> viewportTiles[VIEWPORT_W][VIEWPORT_H]) {
    int x, y;

    /*
     * the shadow rasters for each viewport octant
     *
     * shadowRaster[0][0]    // number of raster segments in this shadow
     * shadowRaster[0][1]    // #1 shadow bitmask value (low three bits) + "newline" flag (high bit)
     * shadowRaster[0][2]    // #1 length
     * shadowRaster[0][3]    // #2 shadow bitmask value
     * shadowRaster[0][4]    // #2 length
     * shadowRaster[0][5]    // #3 shadow bitmask value
     * shadowRaster[0][6]    // #3 length
     * ...etc...
     */
    const int shadowRaster[14][13] = {
        { 6, __VCH, 4, _N_CH, 1, __VCH, 3, _N___, 1, ___CH, 1, __VCH, 1 },    // raster_1_0
        { 6, __VC_, 1, _NVCH, 2, __VC_, 1, _NVCH, 3, _NVCH, 2, _NVCH, 1 },    // raster_1_1
        //
        { 4, __VCH, 3, _N__H, 1, ___CH, 1, __VCH, 1,     0, 0,     0, 0 },    // raster_2_0
        { 6, __VC_, 2, _N_CH, 1, __VCH, 2, _N_CH, 1, __VCH, 1, _N__H, 1 },    // raster_2_1
        { 6, __V__, 1, _NVCH, 1, __VC_, 1, _NVCH, 1, __VC_, 1, _NVCH, 1 },    // raster_2_2
        //
        { 2, __VCH, 2, _N__H, 2,     0, 0,     0, 0,     0, 0,     0, 0 },    // raster_3_0
        { 3, __VC_, 2, _N_CH, 1, __VCH, 1,     0, 0,     0, 0,     0, 0 },    // raster_3_1
        { 3, __VC_, 1, _NVCH, 2, _N_CH, 1,     0, 0,     0, 0,     0, 0 },    // raster_3_2
        { 3, _NVCH, 1, __V__, 1, _NVCH, 1,     0, 0,     0, 0,     0, 0 },    // raster_3_3
        //
        { 2, __VCH, 1, _N__H, 1,     0, 0,     0, 0,     0, 0,     0, 0 },    // raster_4_0
        { 2, __VC_, 1, _N__H, 1,     0, 0,     0, 0,     0, 0,     0, 0 },    // raster_4_1
        { 2, __VC_, 1, _N_CH, 1,     0, 0,     0, 0,     0, 0,     0, 0 },    // raster_4_2
        { 2, __V__, 1, _NVCH, 1,     0, 0,     0, 0,     0, 0,     0, 0 },    // raster_4_3
        { 2, __V__, 1, _NVCH, 1,     0, 0,     0, 0,     0, 0,     0, 0 }     // raster_4_4
    };

    /*
     * As each viewport tile is processed, it will store the bitmask for the shadow it casts.
     * Later, after processing all octants, the entire viewport will be marked visible except
     * for those tiles that have the __VCH bitmask.
     */
    const int _OCTANTS = 8;
    const int _NUM_RASTERS_COLS = 4;
    
    int octant;
    int xOrigin, yOrigin, xSign, ySign, reflect, xTile, yTile, xTileOffset, yTileOffset;

    for (octant = 0; octant < _OCTANTS; octant++) {
        switch (octant) {
            case 0:  xSign=  1;  ySign=  1;  reflect=false;  break;        // lower-right
            case 1:  xSign=  1;  ySign=  1;  reflect=true;   break;
            case 2:  xSign=  1;  ySign= -1;  reflect=true;   break;        // lower-left
            case 3:  xSign= -1;  ySign=  1;  reflect=false;  break;
            case 4:  xSign= -1;  ySign= -1;  reflect=false;  break;        // upper-left
            case 5:  xSign= -1;  ySign= -1;  reflect=true;   break;
            case 6:  xSign= -1;  ySign=  1;  reflect=true;   break;        // upper-right
            case 7:  xSign=  1;  ySign= -1;  reflect=false;  break;
        }

        // determine the origin point for the current LOS octant
        xOrigin = VIEWPORT_W / 2;
        yOrigin = VIEWPORT_H / 2;

        // make sure the segment doesn't reach out of bounds
        int maxWidth      = xOrigin;
        int maxHeight     = yOrigin;
        int currentRaster = 0;

        // just in case the viewport isn't square, swap the width and height
        if (reflect) {
            // swap height and width for later use
            maxWidth ^= maxHeight;
            maxHeight ^= maxWidth;
            maxWidth ^= maxHeight;
        }

        // check the visibility of each tile
        for (int currentCol = 1; currentCol <= _NUM_RASTERS_COLS; currentCol++) {
            for (int currentRow = 0; currentRow <= currentCol; currentRow++) {
                // swap X and Y to reflect the octant rasters
                if (reflect) {
                    xTile = xOrigin+(currentRow*ySign);
                    yTile = yOrigin+(currentCol*xSign);
                }
                else {
                    xTile = xOrigin+(currentCol*xSign);
                    yTile = yOrigin+(currentRow*ySign);
                }

                if (viewportTiles[xTile][yTile].front().getTileType()->isOpaque()) {
                    // a wall was detected, so go through the raster for this wall
                    // segment and mark everything behind it with the appropriate
                    // shadow bitmask.
                    //
                    // first, get the correct raster
                    //
                    if ((currentCol==1) && (currentRow==0)) { currentRaster=0; }
                    else if ((currentCol==1) && (currentRow==1)) { currentRaster=1; }
                    else if ((currentCol==2) && (currentRow==0)) { currentRaster=2; }
                    else if ((currentCol==2) && (currentRow==1)) { currentRaster=3; }
                    else if ((currentCol==2) && (currentRow==2)) { currentRaster=4; }
                    else if ((currentCol==3) && (currentRow==0)) { currentRaster=5; }
                    else if ((currentCol==3) && (currentRow==1)) { currentRaster=6; }
                    else if ((currentCol==3) && (currentRow==2)) { currentRaster=7; }
                    else if ((currentCol==3) && (currentRow==3)) { currentRaster=8; }
                    else if ((currentCol==4) && (currentRow==0)) { currentRaster=9; }
                    else if ((currentCol==4) && (currentRow==1)) { currentRaster=10; }
                    else if ((currentCol==4) && (currentRow==2)) { currentRaster=11; }
                    else if ((currentCol==4) && (currentRow==3)) { currentRaster=12; }
                    else { currentRaster=13; }  // currentCol and currentRow must equal 4

                    xTileOffset = 0;
                    yTileOffset = 0;

                    //========================================
                    for (int currentSegment = 0; currentSegment < shadowRaster[currentRaster][0]; currentSegment++) {
                        // each shadow segment is 2 bytes
                        int shadowType   = shadowRaster[currentRaster][currentSegment*2+1];
                        int shadowLength = shadowRaster[currentRaster][currentSegment*2+2];

                        // update the raster length to make sure it fits in the viewport
                        shadowLength = (shadowLength+1+yTileOffset > maxWidth ? maxWidth : shadowLength);

                        // check to see if we should move up a row
                        if (shadowType & 0x80) {
                            // remove the flag from the shadowType
                            shadowType ^= _N___;
//                            if (currentRow + yTileOffset >= maxHeight) {
                            if (currentRow + yTileOffset > maxHeight) {
                                break;
                            }
                            xTileOffset = yTileOffset;
                            yTileOffset++;
                        }

                        /* it is seemingly unnecessary to swap the edges for
                         * shadow tiles, because we only care about shadow
                         * tiles that have all three parts (V, C, and H)
                         * flagged.  if a tile has fewer than three, it is
                         * ignored during the draw phase, so vertical and
                         * horizontal shadow edge accuracy isn't important
                         */
                        // if reflecting the octant, swap the edges
//                        if (reflect) {
//                            int shadowTemp = 0;
//                            // swap the vertical and horizontal shadow edges
//                            if (shadowType & __V__) { shadowTemp |= ____H; }
//                            if (shadowType & ___C_) { shadowTemp |= ___C_; }
//                            if (shadowType & ____H) { shadowTemp |= __V__; }
//                            shadowType = shadowTemp;
//                        }

                        for (int currentShadow = 1; currentShadow <= shadowLength; currentShadow++) {
                            // apply the shadow to the shadowMap
                            if (reflect) {
                                screenLos[xTile + ((yTileOffset) * ySign)][yTile + ((currentShadow+xTileOffset) * xSign)] |= shadowType;
                            }
                            else {
                                screenLos[xTile + ((currentShadow+xTileOffset) * xSign)][yTile + ((yTileOffset) * ySign)] |= shadowType;
                            }
                        }
                        xTileOffset += shadowLength;
                    }  // for (int currentSegment = 0; currentSegment < shadowRaster[currentRaster][0]; currentSegment++)
                    //========================================

                }  // if (viewportTiles[xTile][yTile].front().getTileType()->isOpaque())
            }  // for (int currentRow = 0; currentRow <= currentCol; currentRow++)
        }  // for (int currentCol = 1; currentCol <= _NUM_RASTERS_COLS; currentCol++)
    }  // for (octant = 0; octant < _OCTANTS; octant++)

    // go through all tiles on the viewable area and set the appropriate visibility
    for (y = 0; y < VIEWPORT_H; y++) {
        for (x = 0; x < VIEWPORT_W; x++) {
            // if the shadow flags equal __VCH, hide it, otherwise it's fully visible
            //
            if ((screenLos[x][y] & __VCH) == __VCH) {
                screenLos[x][y] = 0;
            }
            else {
                screenLos[x][y] = 1;
            }
        }
    }
}

void screenRedrawMapArea() {
    game->mapArea.update();
}

void screenEraseMapArea() {
    Image *screen = imageMgr->get("screen")->image;
    screen->fillRect(BORDER_WIDTH * settings.scale,
                     BORDER_WIDTH * settings.scale,
                     VIEWPORT_W * TILE_WIDTH * settings.scale,
                     VIEWPORT_H * TILE_HEIGHT * settings.scale,
                     0, 0, 0, 255);
}

void screenEraseTextArea(int x, int y, int width, int height) {
    Image *screen = imageMgr->get("screen")->image;
    screen->fillRect(x * CHAR_WIDTH * settings.scale,
                     y * CHAR_HEIGHT * settings.scale,
                     width * CHAR_WIDTH * settings.scale,
                     height * CHAR_HEIGHT * settings.scale,
                     0, 0, 0, 255);
}

/**
 * Do the tremor spell effect where the screen shakes.
 */
void screenShake(int iterations) {
    int shakeOffset;
    unsigned short i;
    Image *screen = imageMgr->get("screen")->image;
    Image *bottom;
    
    // the MSVC8 binary was generating a Access Violation when using
    // drawSubRectOn() or drawOn() to draw the screen surface on top
    // of itself.  Occured on settings.scale 2 and 4 only.
    // Therefore, a temporary Image buffer is used to store the area
    // that gets clipped at the bottom.
    
    if (settings.screenShakes) {
        // specify the size of the offset, and create a buffer
        // to store the offset row plus 1
        shakeOffset = 1;
        bottom = Image::create(SCALED(320), SCALED(shakeOffset+1));
        
        for (i = 0; i < iterations; i++) {
            // store the bottom row
            screen->drawOn(bottom, 0, SCALED((shakeOffset+1)-200));
            
            // shift the screen down and make the top row black
            screen->drawSubRectOn(screen, 0, SCALED(shakeOffset), 0, 0, SCALED(320), SCALED(200-(shakeOffset+1)));
            bottom->drawOn(screen, 0, SCALED(200-(shakeOffset)));
            screen->fillRect(0, 0, SCALED(320), SCALED(shakeOffset), 0, 0, 0, 255);
            screenRedrawScreen();
            EventHandler::sleep(settings.shakeInterval);
            
            // shift the screen back up, and replace the bottom row
            screen->drawOn(screen, 0, 0-SCALED(shakeOffset));
            bottom->drawOn(screen, 0, SCALED(200-(shakeOffset+1)));
            screenRedrawScreen();
            EventHandler::sleep(settings.shakeInterval);
        }
        // free the bottom row image
        delete bottom;
    }
}

/**
 * Draw a tile graphic on the screen.
 */
void screenShowGemTile(Layout *layout, Map *map, MapTile &t, bool focus, int x, int y) {
    // Make sure we account for tiles that look like other tiles (dungeon tiles, mainly)
    string looks_like = t.getTileType()->getLooksLike();
    if (!looks_like.empty())
        t = map->tileset->getByName(looks_like)->getId();
    
    unsigned int tile = map->translateToRawTileIndex(t);
    
    if (map->type == Map::DUNGEON) {
        xu4_assert(charsetInfo, "charset not initialized");
        std::map<string, int>::iterator charIndex = dungeonTileChars.find(t.getTileType()->getName());
        if (charIndex != dungeonTileChars.end()) {
            charsetInfo->image->drawSubRect((layout->viewport.x + (x * layout->tileshape.width)) * settings.scale,
                                            (layout->viewport.y + (y * layout->tileshape.height)) * settings.scale,
                                            0, 
                                            charIndex->second * layout->tileshape.height * settings.scale, 
                                            layout->tileshape.width * settings.scale,
                                            layout->tileshape.height * settings.scale);
        }
    }
    else {
        if (gemTilesInfo == NULL) {
            gemTilesInfo = imageMgr->get(BKGD_GEMTILES);
            if (!gemTilesInfo)
                xu4_error(XU4_LOG_ERR, "ERROR 1002: Unable to load the \"%s\" data file.\t\n\nIs Ultima IV installed?\n\nVisit the XU4 website for additional information.\n\thttp://xu4.sourceforge.net/", BKGD_GEMTILES);
        }
        
        if (tile < 128) {
            gemTilesInfo->image->drawSubRect((layout->viewport.x + (x * layout->tileshape.width)) * settings.scale,
                                             (layout->viewport.y + (y * layout->tileshape.height)) * settings.scale,
                                             0, 
                                             tile * layout->tileshape.height * settings.scale,
                                             layout->tileshape.width * settings.scale,
                                             layout->tileshape.height * settings.scale);
        } else {
            Image *screen = imageMgr->get("screen")->image;
            screen->fillRect((layout->viewport.x + (x * layout->tileshape.width)) * settings.scale,
                             (layout->viewport.y + (y * layout->tileshape.height)) * settings.scale,
                             layout->tileshape.width * settings.scale,
                             layout->tileshape.height * settings.scale,
                             0, 0, 0, 255);
        }
    }
}

Layout *screenGetGemLayout(const Map *map) {
    if (map->type == Map::DUNGEON) {
        std::vector<Layout *>::const_iterator i;
        for (i = layouts.begin(); i != layouts.end(); i++) {
            Layout *layout = *i;
            
            if (layout->type == LAYOUT_DUNGEONGEM)
                return layout;
        }
        xu4_error(XU4_LOG_ERR, "no dungeon gem layout found!\n");
        return NULL;
    }
    else
        return gemlayout;
}


void screenGemUpdate() {
    MapTile tile;
    int x, y;
    Image *screen = imageMgr->get("screen")->image;
    
    screen->fillRect(BORDER_WIDTH * settings.scale, 
                     BORDER_HEIGHT * settings.scale,
                     VIEWPORT_W * TILE_WIDTH * settings.scale, 
                     VIEWPORT_H * TILE_HEIGHT * settings.scale,
                     0, 0, 0, 255);
    
    Layout *layout = screenGetGemLayout(c->location->map);
    
    
    //TODO, move the code responsible for determining 'peer' visibility to a non SDL specific part of the code.
    if (c->location->map->type == Map::DUNGEON) {
    	//DO THE SPECIAL DUNGEON MAP TRAVERSAL
    	std::vector<std::vector<int> > drawnTiles(layout->viewport.width, vector<int>(layout->viewport.height, 0));
    	std::list<std::pair<int,int> > coordStack;
        
    	//Put the avatar's position on the stack
    	int center_x = layout->viewport.width / 2 - 1;
    	int center_y = layout->viewport.height / 2 - 1;
    	int avt_x = c->location->coords.x - 1;
    	int avt_y = c->location->coords.y - 1;
        
    	coordStack.push_back(std::pair<int,int>(center_x, center_y));
    	bool weAreDrawingTheAvatarTile = true;
        
    	//And draw each tile on the growing stack until it is empty
    	while (coordStack.size() > 0) {
    		std::pair<int,int> currentXY = coordStack.back();
    		coordStack.pop_back();
            
    		x = currentXY.first;
    		y = currentXY.second;
            
    		if (	x < 0 || x >= layout->viewport.width ||
                y < 0 || y >= layout->viewport.height)
    			continue;	//Skip out of range tiles
            
    		if (drawnTiles[x][y])
    			continue;	//Skip already considered tiles
            
    		drawnTiles[x][y] = 1;
            
    		// DRAW THE ACTUAL TILE
    		bool focus;
            
            
			vector<MapTile> tiles = screenViewportTile(layout->viewport.width,
                                                       layout->viewport.height, x - center_x + avt_x, y - center_y + avt_y, focus);
			tile = tiles.front();
            
			TileId avatarTileId = c->location->map->tileset->getByName("avatar")->getId();
            
            
			if (!weAreDrawingTheAvatarTile)
			{
				//Hack to avoid showing the avatar tile multiple times in cycling dungeon maps
				if (tile.getId() == avatarTileId)
					tile = c->location->map->getTileFromData(c->location->coords)->getId();
			}
            
			screenShowGemTile(layout, c->location->map, tile, focus, x, y);
            
			if (!tile.getTileType()->isOpaque() || tile.getTileType()->isWalkable() ||  weAreDrawingTheAvatarTile)
			{
				//Continue the search so we can see through all walkable objects, non-opaque objects (like creatures)
				//or the avatar position in those rare circumstances where he is stuck in a wall
                
				//by adding all relative adjacency combinations to the stack for drawing
				coordStack.push_back(std::pair<int,int>(x	+ 1	,	y	- 1	));
				coordStack.push_back(std::pair<int,int>(x	+ 1	,	y		));
				coordStack.push_back(std::pair<int,int>(x	+ 1	,	y	+ 1	));
                
				coordStack.push_back(std::pair<int,int>(x		,	y	- 1	));
				coordStack.push_back(std::pair<int,int>(x		,	y	+ 1	));
                
				coordStack.push_back(std::pair<int,int>(x	- 1	,	y	- 1	));
				coordStack.push_back(std::pair<int,int>(x	- 1	,	y	 	));
				coordStack.push_back(std::pair<int,int>(x	- 1	,	y	+ 1	));
                
				// We only draw the avatar tile once, it is the first tile drawn
				weAreDrawingTheAvatarTile = false;
			}
    	}
        
	} else {
		//DO THE REGULAR EVERYTHING-IS-VISIBLE MAP TRAVERSAL
		for (x = 0; x < layout->viewport.width; x++) {
			for (y = 0; y < layout->viewport.height; y++) {
				bool focus;
				tile = screenViewportTile(layout->viewport.width,
                                          layout->viewport.height, x, y, focus).front();
				screenShowGemTile(layout, c->location->map, tile, focus, x, y);
			}
		}
	}
    
    screenRedrawMapArea();
    
    screenUpdateCursor();
    screenUpdateMoons();
    screenUpdateWind();
}
