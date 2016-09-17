/*!
 * \file src/font.c
 *
 * \brief Functions for loading and manipulating fonts
 *
 * \todo We currently hardcode the grid and PCB size.\n
 * What we should do in the future is scan the font for its extents, and
 * size the grid appropriately.\n
 * Also, when we convert back to a font, we should search the grid for
 * the gridlines and use them to figure out where the symbols are.
 *
 * <hr>
 *
 * <h1><b>Copyright.</b></h1>\n
 *
 * PCB, interactive printed circuit board design
 *
 * Copyright (C) 2006 DJ Delorie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Contact addresses for paper mail and Email:
 * DJ Delorie, 334 North Road, Deerfield NH 03037-1110, USA
 * dj@delorie.com
 */

#include <glib.h>
#include <string.h>     // memset

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "global.h"

#include "font.h"

#include "create.h"     // CreateNewLineInSymbol
#include "const.h"      // MIL_TO_COORD
#include "data.h"       // Settings, PCB
#include "error.h"      // Message
#include "misc.h"       // MoveLayerToGroup, NoFlags, SetFontInfo
#include "move.h"       // MoveLayer
#include "hid.h"        // hid_action, hid_actionl
#include "parse_l.h"    // ParseFont
#include "pcb-printf.h" // get_unit_struct

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

GSList * SystemFontLibrary = NULL;


int
check_font_name(FontType * font, char * name)
{
    return g_strcmp0(font->Name, name);
}
/*!
 * \brief Finds a font by name in the system font library
 */
FontType *
FindFont(char * fontname)
{
    GSList * font = g_slist_find_custom(SystemFontLibrary,
                                        fontname,
                                        (GCompareFunc)check_font_name);
    if (font) return font->data;
    return NULL;
}

/*!
 * \brief Finds a font by name in the system font library
 */
FontType *
ChangeFont(char * fontname)
{
    FontType * font = FindFont(fontname);
    if (!font)
    {
        Message(_("Could not change font to %s because it isn't in the library\n"
               "Leaving the font set to %s.\n"), fontname, Settings.Font->Name);
        return NULL;
    }
    Message(_("Switching to font %s\n"), fontname);
    Settings.Font = font;
    return font;
}

/*!
 * \brief Loads a new font from a file
 */
FontType *
LoadFont(char * filename)
{
    // This will successfully load and install the new font, but it will change
    // the font of all text on the board. Text that was already there also changes
    // to the new font the next time the screen is redrawn.
    FontType * newfont;
    if (FindFont(filename)) {
        Message(_("Font %s already loaded. Switching to it.\n"), filename);
        return ChangeFont(filename);
    }
    newfont = g_new(FontType, 1);
    memset(newfont->Symbol, 0, sizeof(newfont->Symbol));
    if (ParseFont(newfont, filename)){
        Message(_("Failed to load font file '%s'\n"), filename);
        g_free(newfont);
        return NULL;
    }
    Settings.Font = newfont;
    Settings.Font->Name = g_strdup(filename);
    SystemFontLibrary = g_slist_append(SystemFontLibrary, newfont);
    Message(_("New font loaded: %s\n"), Settings.Font->Name);
    return Settings.Font;
}

int
FreeFontMemory(FontType * font)
{
    int i;
    Message(_("Freeing font memory: %s\n"), font->Name);
    /* Free any lines allocated to font symbols */
    for (i = 0; i <= MAX_FONTPOSITION; i++) free (font->Symbol[i].Line);
    /* Release the memory holding the font name */
    free(font->Name);
    /* Free the memory holding the font structure itself */
    free(font);
    return 0;
}

int
UnloadFont(char * fontname)
{
    FontType * font;
    if (g_strcmp0(fontname, "all") == 0)
    {
        g_slist_foreach(SystemFontLibrary, (GFunc)FreeFontMemory, NULL);
        g_slist_free(SystemFontLibrary);
        SystemFontLibrary = NULL;
        Settings.Font = NULL;
    } else
    {
        font = FindFont(fontname);
        if (!font)
        {
            Message(_("Could not unload font %s, font not found in list\n"),
                   fontname);
            return -1;
        }
        FreeFontMemory(font);
        SystemFontLibrary = g_slist_remove(SystemFontLibrary, font);
        /* 
         * if we unloaded the current font, either switch to the first one in
         * the list, or if there are no more, set the font pointer to NULL.
         */
        if (font == Settings.Font)
        {
            if (g_slist_length(SystemFontLibrary) >= 1)
                ChangeFont(((FontType*)SystemFontLibrary->data)->Name);
            else
                Settings.Font = NULL;
        }
    }
    return 0;
}

static const char changefont_syntax[] = "ChangeFonts(fontname)";

static const char changefont_help[] = "Change the current font";

static int
ChangeFontAction(int argc, char **argv, Coord x, Coord y)
{
    if (argc < 1)
    {
        Message (_("Tell me what font use\n"));
        return -1;
    }
    ChangeFont(argv[0]);
    return 0;
}

static const char listfonts_syntax[] = "ListFonts()";

static const char listfonts_help[] = "List fonts in library.";

static int
ListFontsAction(int argc, char **argv, Coord x, Coord y)
{
    GSList * itt;
    Message(_("Fonts in library: \n"));
    for (itt = SystemFontLibrary; itt; itt = itt->next)
        Message(_("\t%s\n"), ((FontType*)itt->data)->Name);
    return 0;
}


static const char loadfont_syntax[] = "LoadFont(filename)";

static const char loadfont_help[] = "Load font data from a file.";
/*!
 * \brief User Action to load a new font
 */
static int
LoadFontAction (int argc, char **argv, Coord x, Coord y)
{
    if (argc < 1)
    {
        Message (_("Tell me what font file to load\n"));
        return -1;
    }
    LoadFont(argv[0]);
    return 0;
}

static const char unloadfont_syntax[] = "UnloadFont(filename)";

static const char unloadfont_help[] = "Unload font data from the system library.";
/*!
 * \brief User Action to load a new font
 */
static int
UnloadFontAction (int argc, char **argv, Coord x, Coord y)
{
    if (argc < 1)
    {
        Message (_("Tell me what font to unload\n"));
        return -1;
    }
    UnloadFont(argv[0]);
    /* 
     * Don't let the user get rid of all the fonts, there has to be at least one
     * in order to create any kind of text object. Do this here, because we do 
     * want to let the system unload all fonts when exiting.
     */
    if (g_slist_length(SystemFontLibrary) < 1)
    {
        Message(_("You have to leave at least one font in the libary.\n"
               "Loading the default font.\n"));
        LoadFont(Settings.FontFile);
    }
    return 0;
}

#define CELL_SIZE	((Coord)MIL_TO_COORD(100))
#define CELL_OFFSET	((Coord)MIL_TO_COORD(10))

#define XYtoSym(x,y) ((x + CELL_OFFSET) / CELL_SIZE - 1 \
		      + 16 * ((y + CELL_OFFSET) / CELL_SIZE - 1))

static const char fontedit_syntax[] = "FontEdit()";

static const char fontedit_help[] =
  "Convert the current font to a PCB for editing.";

/* %start-doc actions FontEdit

This command only allows a font to be edited if the layout being edited contains
font symbols. The existing font symbols are displayed on a layer with an overlaid
grid, with the new version of the font on another layer which can be modified.
Font symbols consist only of lines. The blue lines next to the font symbols
indicate the space required after the symbol when it is used.

%end-doc */

static int
FontEdit (int argc, char **argv, Coord Ux, Coord Uy)
{
  FontType *font;
  SymbolType *symbol;
  LayerType *lfont, *lorig, *lwidth, *lgrid;
  int s, l;

  if (hid_actionl ("New", "Font", 0))
    return 1;

  Settings.grid_unit = get_unit_struct("mil");
  Settings.Bloat = PCB->Bloat = 1;
  Settings.Shrink = PCB->Shrink = 1;
  Settings.minWid = PCB->minWid = 1;
  Settings.minSlk = PCB->minSlk = 1;

  MoveLayerToGroup (max_copper_layer + TOP_SILK_LAYER, 0);
  MoveLayerToGroup (max_copper_layer + BOTTOM_SILK_LAYER, 1);

  while (PCB->Data->LayerN > 4)
    MoveLayer (4, -1);
  for (l = 0; l < 4; l++)
    {
      MoveLayerToGroup (l, l);
    }
  PCB->MaxWidth = CELL_SIZE * 18;
  PCB->MaxHeight = CELL_SIZE * ((MAX_FONTPOSITION + 15) / 16 + 2);
  PCB->Grid = MIL_TO_COORD (5);
  PCB->Data->Layer[0].Name = strdup ("Font");
  PCB->Data->Layer[1].Name = strdup ("OrigFont");
  PCB->Data->Layer[2].Name = strdup ("Width");
  PCB->Data->Layer[3].Name = strdup ("Grid");
  hid_action ("PCBChanged");
  hid_action ("LayersChanged");

  lfont = PCB->Data->Layer + 0;
  lorig = PCB->Data->Layer + 1;
  lwidth = PCB->Data->Layer + 2;
  lgrid = PCB->Data->Layer + 3;

  font = Settings.Font;
  for (s = 0; s <= MAX_FONTPOSITION; s++)
    {
      Coord ox = (s % 16 + 1) * CELL_SIZE;
      Coord oy = (s / 16 + 1) * CELL_SIZE;
      Coord w, miny, maxy, maxx = 0;

      symbol = &font->Symbol[s];

      miny = MIL_TO_COORD (5);
      maxy = font->MaxHeight;

      for (l = 0; l < symbol->LineN; l++)
	{
	  CreateDrawnLineOnLayer (lfont,
				  symbol->Line[l].Point1.X + ox,
				  symbol->Line[l].Point1.Y + oy,
				  symbol->Line[l].Point2.X + ox,
				  symbol->Line[l].Point2.Y + oy,
				  symbol->Line[l].Thickness,
				  symbol->Line[l].Thickness, NoFlags ());
	  CreateDrawnLineOnLayer (lorig, symbol->Line[l].Point1.X + ox,
				  symbol->Line[l].Point1.Y + oy,
				  symbol->Line[l].Point2.X + ox,
				  symbol->Line[l].Point2.Y + oy,
				  symbol->Line[l].Thickness,
				  symbol->Line[l].Thickness, NoFlags ());
	  if (maxx < symbol->Line[l].Point1.X)
	    maxx = symbol->Line[l].Point1.X;
	  if (maxx < symbol->Line[l].Point2.X)
	    maxx = symbol->Line[l].Point2.X;
	}
      w = maxx + symbol->Delta + ox;
      CreateDrawnLineOnLayer (lwidth,
			      w, miny + oy,
			      w, maxy + oy, MIL_TO_COORD (1), MIL_TO_COORD (1), NoFlags ());
    }

  for (l = 0; l < 16; l++)
    {
      int x = (l + 1) * CELL_SIZE;
      CreateDrawnLineOnLayer (lgrid, x, 0, x, PCB->MaxHeight, MIL_TO_COORD (1),
                              MIL_TO_COORD (1), NoFlags ());
    }
  for (l = 0; l <= MAX_FONTPOSITION / 16 + 1; l++)
    {
      int y = (l + 1) * CELL_SIZE;
      CreateDrawnLineOnLayer (lgrid, 0, y, PCB->MaxWidth, y, MIL_TO_COORD (1),
                              MIL_TO_COORD (1), NoFlags ());
    }
  return 0;
}

static const char fontsave_syntax[] = "FontSave()";

static const char fontsave_help[] = "Convert the current PCB back to a font.";

/* %start-doc actions FontSave

Once a font has been modified with the FontEdit command, the layout can be saved
as a new PCB layout. The new PCB layout can then be opened with a text editor so
that the font section can be removed and saved as a new "default_font" file for
use in other PCB layouts.

%end-doc */

static int
FontSave (int argc, char **argv, Coord Ux, Coord Uy)
{
  FontType *font;
  SymbolType *symbol;
  int i;
  GList *ii;
  LayerType *lfont, *lwidth;

  font = Settings.Font;
  lfont = PCB->Data->Layer + 0;
  lwidth = PCB->Data->Layer + 2;

  for (i = 0; i <= MAX_FONTPOSITION; i++)
    {
      font->Symbol[i].LineN = 0;
      font->Symbol[i].Valid = 0;
      font->Symbol[i].Width = 0;
    }

  for (ii = lfont->Line; ii != NULL; ii = g_list_next (ii))
    {
      LineType *l = ii->data;
      Coord x1 = l->Point1.X;
      Coord y1 = l->Point1.Y;
      Coord x2 = l->Point2.X;
      Coord y2 = l->Point2.Y;
      Coord ox, oy;
      int s;

      s = XYtoSym (x1, y1);
      ox = (s % 16 + 1) * CELL_SIZE;
      oy = (s / 16 + 1) * CELL_SIZE;
      symbol = &Settings.Font->Symbol[s];

      x1 -= ox;
      y1 -= oy;
      x2 -= ox;
      y2 -= oy;

      if (symbol->Width < x1)
	symbol->Width = x1;
      if (symbol->Width < x2)
	symbol->Width = x2;
      symbol->Valid = 1;

      CreateNewLineInSymbol (symbol, x1, y1, x2, y2, l->Thickness);
    }

  for (ii = lwidth->Line; ii != NULL; ii = g_list_next (ii))
    {
      LineType *l = ii->data;
      Coord x1 = l->Point1.X;
      Coord y1 = l->Point1.Y;
      Coord ox, s;

      s = XYtoSym (x1, y1);
      ox = (s % 16 + 1) * CELL_SIZE;
      symbol = &Settings.Font->Symbol[s];

      x1 -= ox;

      symbol->Delta = x1 - symbol->Width;
    }

  SetFontInfo (font);
  
  return 0;
}

HID_Action fontmode_action_list[] = {
  {"ChangeFont", 0, ChangeFontAction,
   changefont_help, changefont_syntax},
  {"LoadFont", 0, LoadFontAction,
   loadfont_help, loadfont_syntax},
  {"UnloadFont", 0, UnloadFontAction,
   unloadfont_help, unloadfont_syntax},
  {"ListFonts", 0, ListFontsAction,
   listfonts_help, listfonts_syntax},
  {"FontEdit", 0, FontEdit,
   fontedit_help, fontedit_syntax},
  {"FontSave", 0, FontSave,
   fontsave_help, fontsave_syntax}
};

REGISTER_ACTIONS (fontmode_action_list)
