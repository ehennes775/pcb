/*!
 * \file src/find.c
 *
 * \brief Routines to find connections between pins, vias, lines ...
 *
 * Short description:\n
 * <ul>
 * <li> Lists for pins and vias, lines, arcs, pads and for polygons are
 *   created.\n
 *   Every object that has to be checked is added to its list.\n
 *   Coarse searching is accomplished with the data rtrees.</li>
 * <li> There's no 'speed-up' mechanism for polygons because they are
 *   not used as often as other objects.</li> 
 * </ul>
 *
 * PV:  means pin or via (objects that connect layers).\n
 * LO:  all non PV objects (layer objects like lines, arcs, polygons,
 * pads).
 *
 * <ol>
 * <li> First, the LO or PV at the given coordinates is looked
 *   up.</li>
 * <li> All LO connections to that PV are looked up next.</li>
 * <li> Lookup of all LOs connected to LOs from (2).\n
 *   This step is repeated until no more new connections are found.</li>
 * <li> Lookup all PVs connected to the LOs from (2) and (3).</li>
 * <li> Start again with (1) for all new PVs from (4).</li>
 * </ol>
 *
 * Intersection of line <--> line:\n
 * <ul>
 * <li> See the description of 'LineLineIntersect()'.</li>
 * </ul>
 *
 * <hr>
 *
 * <h1><b>Copyright.</b></h1>\n
 *
 * PCB, interactive printed circuit board design
 *
 * Copyright (C) 1994,1995,1996, 2005 Thomas Nau
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact addresses for paper mail and Email:
 * Thomas Nau, Schlehenweg 15, 88471 Baustetten, Germany
 * Thomas.Nau@rz.uni-ulm.de
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <setjmp.h>
#include <assert.h>

#include "global.h"

#include "data.h"
#include "draw.h"
#include "error.h"
#include "find.h"
#include "misc.h"
#include "rtree.h"
#include "polygon.h"
#include "pcb_geometry.h"
#include "pcb-printf.h"
#include "search.h"
#include "set.h"
#include "undo.h"
#include "rats.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

#undef DEBUG

/* ---------------------------------------------------------------------------
 * some local macros
 */

#define	SEPARATE(FP)							\
	{											\
		int	i;									\
		fputc('#', (FP));						\
		for (i = Settings.CharPerLine; i; i--)	\
			fputc('=', (FP));					\
		fputc('\n', (FP));						\
	}

#define LIST_ENTRY(list,I)      (((AnyObjectType **)list->Data)[(I)])
#define PADLIST_ENTRY(L,I)      (((PadType **)PadList[(L)].Data)[(I)])
#define LINELIST_ENTRY(L,I)     (((LineType **)LineList[(L)].Data)[(I)])
#define ARCLIST_ENTRY(L,I)      (((ArcType **)ArcList[(L)].Data)[(I)])
#define RATLIST_ENTRY(I)        (((RatType **)RatList.Data)[(I)])
#define POLYGONLIST_ENTRY(L,I)  (((PolygonType **)PolygonList[(L)].Data)[(I)])
#define PVLIST_ENTRY(I)         (((PinType **)PVList.Data)[(I)])

#define IS_PV_ON_RAT(PV, Rat) \
	(IsPointOnLineEnd((PV)->X,(PV)->Y, (Rat)))

#define IS_PV_ON_ARC(PV, Arc, pii)	\
	(TEST_FLAG(SQUAREFLAG, (PV)) ? \
		IsArcInRectangle( \
			(PV)->X -MAX(((PV)->Thickness+1)/2 +Bloat,0), (PV)->Y -MAX(((PV)->Thickness+1)/2 +Bloat,0), \
			(PV)->X +MAX(((PV)->Thickness+1)/2 +Bloat,0), (PV)->Y +MAX(((PV)->Thickness+1)/2 +Bloat,0), \
			(Arc), \
                        (pii)) : \
		IsPointOnArc( \
                        (PV)->X,(PV)->Y,MAX((PV)->Thickness/2.0 + Bloat,0.0), \
                        (Arc), \
                        (pii)))

#define	IS_PV_ON_PAD(PV, Pad, pii) \
	( IsPointInPad((PV)->X, (PV)->Y, MAX((PV)->Thickness/2 +Bloat,0), (Pad), (pii)))


static DrcViolationType
*pcb_drc_violation_new (const char *title,
                        const char *explanation,
                        Coord x, Coord y,
                        Angle angle,
                        bool have_measured,
                        Coord measured_value,
                        Coord required_value,
                        int object_count,
                        long int *object_id_list,
                        int *object_type_list)
{
  DrcViolationType *violation = (DrcViolationType *)malloc (sizeof (DrcViolationType));

  violation->title = strdup (title);
  violation->explanation = strdup (explanation);
  violation->x = x;
  violation->y = y;
  violation->angle = angle;
  violation->have_measured = have_measured;
  violation->measured_value = measured_value;
  violation->required_value = required_value;
  violation->object_count = object_count;
  violation->object_id_list = object_id_list;
  violation->object_type_list = object_type_list;

  return violation;
}

static void
pcb_drc_violation_free (DrcViolationType *violation)
{
  free (violation->title);
  free (violation->explanation);
  free (violation);
}

static GString *drc_dialog_message;
static void
reset_drc_dialog_message(void)
{
  if (drc_dialog_message)
    g_string_free (drc_dialog_message, FALSE);
  drc_dialog_message = g_string_new ("");
  if (gui->drc_gui != NULL)
    {
      gui->drc_gui->reset_drc_dialog_message ();
    }
}
static void
append_drc_dialog_message(const char *fmt, ...)
{
  gchar *new_str;
  va_list ap;
  va_start (ap, fmt);
  new_str = pcb_vprintf (fmt, ap);
  g_string_append (drc_dialog_message, new_str);
  va_end (ap);
  g_free (new_str);
}

static void GotoError (void);

static void
append_drc_violation (DrcViolationType *violation)
{
  if (gui->drc_gui != NULL)
    {
      gui->drc_gui->append_drc_violation (violation);
    }
  else
    {
      /* Fallback to formatting the violation message as text */
      append_drc_dialog_message ("%s\n", violation->title);
      append_drc_dialog_message (_("%m+near %$mD\n"),
                                 Settings.grid_unit->allow,
                                 violation->x, violation->y);
      GotoError ();
    }

  if (gui->drc_gui == NULL || gui->drc_gui->log_drc_violations )
    {
      Message (_("WARNING!  Design Rule error - %s\n"), violation->title);
      Message (_("%m+near location %$mD\n"),
               Settings.grid_unit->allow,
               violation->x, violation->y);
    }
}

#define DRC_CONTINUE _("Press Next to continue DRC checking")
#define DRC_NEXT _("Next")
#define DRC_CANCEL _("Cancel")

/*!
 * \brief Message when asked about continuing DRC checks after next 
 * violation is found.
 */
static int
throw_drc_dialog(void)
{
  int r;

  if (gui->drc_gui != NULL)
    {
      r = gui->drc_gui->throw_drc_dialog ();
    }
  else
    {
      /* Fallback to formatting the violation message as text */
      append_drc_dialog_message (DRC_CONTINUE);
      r = gui->confirm_dialog (drc_dialog_message->str, DRC_CANCEL, DRC_NEXT);
      reset_drc_dialog_message();
    }
  return r;
}

/*!
 * \brief Some local types.
 *
 * The two 'dummy' structs for PVs and Pads are necessary for creating
 * connection lists which include the element's name.
 */
typedef struct
{
  void **Data;                  /*!< Pointer to index data. */
  Cardinal Location,            /*!< Currently used position. */
    DrawLocation, Number,       /*!< Number of objects in list. */
    Size;
} ListType;

/* ---------------------------------------------------------------------------
 * some local identifiers
 */
static Coord Bloat = 0;
static void *thing_ptr1, *thing_ptr2, *thing_ptr3;
static int thing_type;
static bool User = false;    /*!< User action causing this. */
static bool drc = false;     /*!< Whether to stop if finding something not found. */
static Cardinal drcerr_count;   /*!< Count of drc errors */
static Cardinal TotalP, TotalV;
static ListType LineList[MAX_LAYER],    /*!< List of objects to. */
  PolygonList[MAX_LAYER], ArcList[MAX_LAYER], PadList[2], RatList, PVList;

/* Magic number meaning that we don't have the most recent intersection point.
 * If any field of pimri is set to this, the whole value is invalid.  */
#define PIMRI_UNSET -2

/* Point In Most Recent Intersection.  Currently this is just used for
 * reporting the position of DRC violations involing intersections. */
static PointType pimri
  = { PIMRI_UNSET, PIMRI_UNSET, PIMRI_UNSET, PIMRI_UNSET }; 

/* ---------------------------------------------------------------------------
 * some local prototypes
 */
static bool LookupLOConnectionsToLine (LineType *, Cardinal, int, bool, bool);
static bool LookupLOConnectionsToPad (PadType *, Cardinal, int, bool);
static bool LookupLOConnectionsToPolygon (PolygonType *, Cardinal, int, bool);
static bool LookupLOConnectionsToArc (ArcType *, Cardinal, int, bool);
static bool LookupLOConnectionsToRatEnd (PointType *, Cardinal, int);
static bool IsRatPointOnLineEnd (PointType *, LineType *);
static bool ArcArcIntersect (ArcType *, ArcType *, PointType *pii);
static bool PrepareNextLoop (FILE *);
static void DrawNewConnections (void);
static void DumpList (void);
static void LocateErrorObject (Coord *, Coord *);
static void BuildObjectList (int *, long int **, int **);
static bool SetThing (int, void *, void *, void *);
static bool IsArcInPolygon (ArcType *, PolygonType *);
static bool IsLineInPolygon (LineType *, PolygonType *);
static bool IsPadInPolygon (PadType *, PolygonType *);
static bool IsPolygonInPolygon (PolygonType *, PolygonType *);

/*!
 * \brief.
 *
 * Some of the 'pad' routines are the same as for lines because the 'pad'
 * struct starts with a line struct. See global.h for details.
 */
bool
LinePadIntersect (LineType *Line, PadType *Pad, PointType *pii)
{
  return LineLineIntersect ((Line), (LineType *)Pad, pii);
}

bool
ArcPadIntersect (ArcType *Arc, PadType *Pad, PointType *pii)
{
  return LineArcIntersect ((LineType *) (Pad), (Arc), pii);
}

static bool
add_object_to_list (ListType *list, int type, void *ptr1, void *ptr2, void *ptr3, int flag)
{
  AnyObjectType *object = (AnyObjectType *)ptr2;

  if (User)
    AddObjectToFlagUndoList (type, ptr1, ptr2, ptr3);

  SET_FLAG (flag, object);
  LIST_ENTRY (list, list->Number) = object;
  list->Number++;

#ifdef DEBUG
  if (list.Number > list.Size)
    printf ("add_object_to_list overflow! type=%i num=%d size=%d\n", type, list.Number, list.Size);
#endif

  if (drc && !TEST_FLAG (SELECTEDFLAG, object))
    return (SetThing (type, ptr1, ptr2, ptr3));
  return false;
}

static bool
ADD_PV_TO_LIST (PinType *Pin, int flag)
{
  return add_object_to_list (&PVList, Pin->Element ? PIN_TYPE : VIA_TYPE,
                             Pin->Element ? Pin->Element : Pin, Pin, Pin, flag);
}

static bool
ADD_PAD_TO_LIST (Cardinal L, PadType *Pad, int flag)
{
  return add_object_to_list (&PadList[L], PAD_TYPE, Pad->Element, Pad, Pad, flag);
}

static bool
ADD_LINE_TO_LIST (Cardinal L, LineType *Ptr, int flag)
{
  return add_object_to_list (&LineList[L], LINE_TYPE, LAYER_PTR (L), Ptr, Ptr, flag);
}

static bool
ADD_ARC_TO_LIST (Cardinal L, ArcType *Ptr, int flag)
{
  return add_object_to_list (&ArcList[L], ARC_TYPE, LAYER_PTR (L), Ptr, Ptr, flag);
}

static bool
ADD_RAT_TO_LIST (RatType *Ptr, int flag)
{
  return add_object_to_list (&RatList, RATLINE_TYPE, Ptr, Ptr, Ptr, flag);
}

static bool
ADD_POLYGON_TO_LIST (Cardinal L, PolygonType *Ptr, int flag)
{
  return add_object_to_list (&PolygonList[L], POLYGON_TYPE, LAYER_PTR (L), Ptr, Ptr, flag);
}

static BoxType
expand_bounds (BoxType *box_in)
{
  BoxType box_out = *box_in;

  if (Bloat > 0)
    {
      box_out.X1 -= Bloat;
      box_out.X2 += Bloat;
      box_out.Y1 -= Bloat;
      box_out.Y2 += Bloat;
    }

  return box_out;
}

bool
PinLineIntersect (PinType *PV, LineType *Line, PointType *pii)
{
  /* IsLineInRectangle already has Bloat factor */

  /* FIXME: BUT NOT NOW: so we have all these points where we test for
   * "square" pins.  But we never check for octagonal pins.  pcb will
   * render and produce them, but it doesn't really do the intersection
   * and DRC tests for them completely correctly.  It comes close, since
   * they're almost circular, but the failure can be seen by putting a round
   * line end cap just slightly on one of the octagon verticies and using
   * Connects->Lookup connection (Cntrl-F) on either the line or the pin.
   * The connection isn't detected.  Since it probably only fails for
   * overlaps smaller than any sensible DRC minimum overlap setting this
   * is maybe not worth fixing given the many code points involved.  Still,
   * it's unfortunate to have connection detection not quite catch
   * everything.  */
  
  return 
    TEST_FLAG (SQUAREFLAG, PV) ?
      IsLineInRectangle (
          PV->X - (PIN_SIZE (PV) + 1) / 2,
          PV->Y - (PIN_SIZE (PV) + 1) / 2,
          PV->X + (PIN_SIZE (PV) + 1) / 2,
          PV->Y + (PIN_SIZE (PV) + 1) / 2,
          Line,
          pii ) :
      IsPointInPad (
          PV->X,
          PV->Y,
          MAX (PIN_SIZE (PV) / 2.0 + Bloat, 0.0),
          (PadType *) Line,
          pii );
}


bool
SetThing (int type, void *ptr1, void *ptr2, void *ptr3)
{
  thing_ptr1 = ptr1;
  thing_ptr2 = ptr2;
  thing_ptr3 = ptr3;
  thing_type = type;
  return true;
}

bool
BoxBoxIntersection (BoxType *b1, BoxType *b2, PointType *pii)
{
  if (b2->X2 < b1->X1 || b2->X1 > b1->X2) {
    return false;
  }
  if (b2->Y2 < b1->Y1 || b2->Y1 > b1->Y2) {
    return false;
  }

  /* We have an intersection.  */

  if ( pii != NULL ) {

    /* We'll report the intersection point as the center of area.  */
    Coord ix, iy;   /* Intersection X/Y */

    if ( b1->X1 <= b2->X2 && b2->X2 <= b1->X2 ) {
      ix = b2->X2 - (b2->X2 - b1->X1) / 2;
    }
    else {
      ix = b1->X2 - (b1->X2 - b2->X1) / 2;
    }
    
    if ( b1->Y1 <= b2->Y2 && b2->Y2 <= b1->Y2 ) {
      iy = b2->Y2 - (b2->Y2 - b1->Y1) / 2;
    }
    else {
      iy = b1->Y2 - (b1->Y2 - b2->Y1) / 2;
    }

    pii->X = ix;
    pii->Y = iy;
  }

  return true;
}

static bool
PadPadIntersect (PadType *p1, PadType *p2, PointType *pii)
{
  return LinePadIntersect ((LineType *) p1, p2, pii);
}

static inline bool
PV_TOUCH_PV (PinType *PV1, PinType *PV2, PointType *pii)
{
  double t1, t2;
  BoxType b1, b2;

  t1 = MAX (PV1->Thickness / 2.0 + Bloat, 0);
  t2 = MAX (PV2->Thickness / 2.0 + Bloat, 0);
  if ( IsPointOnPin (PV1->X, PV1->Y, t1, PV2, pii) ) {
    return true;
  }
  if ( IsPointOnPin (PV2->X, PV2->Y, t2, PV1, pii) ) {
    return true;
  }
  if (!TEST_FLAG (SQUAREFLAG, PV1) || !TEST_FLAG (SQUAREFLAG, PV2))
    return false;
  /* check for square/square overlap */
  b1.X1 = PV1->X - t1;
  b1.X2 = PV1->X + t1;
  b1.Y1 = PV1->Y - t1;
  b1.Y2 = PV1->Y + t1;
  t2 = PV2->Thickness / 2.0;
  b2.X1 = PV2->X - t2;
  b2.X2 = PV2->X + t2;
  b2.Y1 = PV2->Y - t2;
  b2.Y2 = PV2->Y + t2;
  return BoxBoxIntersection (&b1, &b2, pii);
}

/*!
 * \brief Releases all allocated memory.
 */
static void
FreeLayoutLookupMemory (void)
{
  Cardinal i;

  for (i = 0; i < max_copper_layer; i++)
    {
      free (LineList[i].Data);
      LineList[i].Data = NULL;
      free (ArcList[i].Data);
      ArcList[i].Data = NULL;
      free (PolygonList[i].Data);
      PolygonList[i].Data = NULL;
    }
  free (PVList.Data);
  PVList.Data = NULL;
  free (RatList.Data);
  RatList.Data = NULL;
}

static void
FreeComponentLookupMemory (void)
{
  free (PadList[0].Data);
  PadList[0].Data = NULL;
  free (PadList[1].Data);
  PadList[1].Data = NULL;
}

/*!
 * \brief Allocates memory for component related stacks ...
 *
 * Initializes index and sorts it by X1 and X2.
 */
static void
InitComponentLookup (void)
{
  Cardinal NumberOfPads[2];
  Cardinal i;

  /* initialize pad data; start by counting the total number
   * on each of the two possible layers
   */
  NumberOfPads[TOP_SIDE] = NumberOfPads[BOTTOM_SIDE] = 0;
  ALLPAD_LOOP (PCB->Data);
  {
    if (TEST_FLAG (ONSOLDERFLAG, pad))
      NumberOfPads[BOTTOM_SIDE]++;
    else
      NumberOfPads[TOP_SIDE]++;
  }
  ENDALL_LOOP;
  for (i = 0; i < 2; i++)
    {
      /* allocate memory for working list */
      PadList[i].Data = (void **)calloc (NumberOfPads[i], sizeof (PadType *));

      /* clear some struct members */
      PadList[i].Location = 0;
      PadList[i].DrawLocation = 0;
      PadList[i].Number = 0;
      PadList[i].Size = NumberOfPads[i];
    }
}

/*!
 * \brief Allocates memory for layout related stacks ...
 *
 * Initializes index and sorts it by X1 and X2.
 */
static void
InitLayoutLookup (void)
{
  Cardinal i;

  /* initialize line arc and polygon data */
  for (i = 0; i < max_copper_layer; i++)
    {
      LayerType *layer = LAYER_PTR (i);

      if (layer->LineN)
        {
          /* allocate memory for line pointer lists */
          LineList[i].Data = (void **)calloc (layer->LineN, sizeof (LineType *));
          LineList[i].Size = layer->LineN;
        }
      if (layer->ArcN)
        {
          ArcList[i].Data = (void **)calloc (layer->ArcN, sizeof (ArcType *));
          ArcList[i].Size = layer->ArcN;
        }


      /* allocate memory for polygon list */
      if (layer->PolygonN)
        {
          PolygonList[i].Data = (void **)calloc (layer->PolygonN, sizeof (PolygonType *));
          PolygonList[i].Size = layer->PolygonN;
        }

      /* clear some struct members */
      LineList[i].Location = 0;
      LineList[i].DrawLocation = 0;
      LineList[i].Number = 0;
      ArcList[i].Location = 0;
      ArcList[i].DrawLocation = 0;
      ArcList[i].Number = 0;
      PolygonList[i].Location = 0;
      PolygonList[i].DrawLocation = 0;
      PolygonList[i].Number = 0;
    }

  if (PCB->Data->pin_tree)
    TotalP = PCB->Data->pin_tree->size;
  else
    TotalP = 0;
  if (PCB->Data->via_tree)
    TotalV = PCB->Data->via_tree->size;
  else
    TotalV = 0;
  /* allocate memory for 'new PV to check' list and clear struct */
  PVList.Data = (void **)calloc (TotalP + TotalV, sizeof (PinType *));
  PVList.Size = TotalP + TotalV;
  PVList.Location = 0;
  PVList.DrawLocation = 0;
  PVList.Number = 0;
  /* Initialize ratline data */
  RatList.Data = (void **)calloc (PCB->Data->RatN, sizeof (RatType *));
  RatList.Size = PCB->Data->RatN;
  RatList.Location = 0;
  RatList.DrawLocation = 0;
  RatList.Number = 0;
}

struct pv_info
{
  Cardinal layer;
  PinType *pv;
  int flag;
  jmp_buf env;
};

static int
LOCtoPVline_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  if (!TEST_FLAG (i->flag, line) && PinLineIntersect (i->pv, line, &pimri) &&
      !TEST_FLAG (HOLEFLAG, i->pv))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPVarc_callback (const BoxType * b, void *cl)
{
  ArcType *arc = (ArcType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  if (!TEST_FLAG (i->flag, arc) && IS_PV_ON_ARC (i->pv, arc, &pimri) &&
      !TEST_FLAG (HOLEFLAG, i->pv))
    {
      if (ADD_ARC_TO_LIST (i->layer, arc, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPVpad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  if (!TEST_FLAG (i->flag, pad) && IS_PV_ON_PAD (i->pv, pad, &pimri) &&
      !TEST_FLAG (HOLEFLAG, i->pv) &&
      ADD_PAD_TO_LIST (TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE :
                       TOP_SIDE, pad, i->flag)) {
    longjmp (i->env, 1);
  }
  return 0;
}

static int
LOCtoPVrat_callback (const BoxType * b, void *cl)
{
  RatType *rat = (RatType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  if (!TEST_FLAG (i->flag, rat) && IS_PV_ON_RAT (i->pv, rat) &&
      ADD_RAT_TO_LIST (rat, i->flag))
    longjmp (i->env, 1);
  return 0;
}
static int
LOCtoPVpoly_callback (const BoxType * b, void *cl)
{
  PolygonType *polygon = (PolygonType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  /* FIXME: BUT NOT NOW: pimri isn't set at all for polygons at the moment */

  /* if the pin doesn't have a therm and polygon is clearing
   * then it can't touch due to clearance, so skip the expensive
   * test. If it does have a therm, you still need to test
   * because it might not be inside the polygon, or it could
   * be on an edge such that it doesn't actually touch.
   */
  if (!TEST_FLAG (i->flag, polygon) && !TEST_FLAG (HOLEFLAG, i->pv) &&
                                       (TEST_THERM (i->layer, i->pv) ||
                                        !TEST_FLAG (CLEARPOLYFLAG,
                                                    polygon)
                                        || !i->pv->Clearance))
    {
      double wide = MAX (0.5 * i->pv->Thickness + Bloat, 0);
      if (TEST_FLAG (SQUAREFLAG, i->pv))
        {
          Coord x1 = i->pv->X - (i->pv->Thickness + 1 + Bloat) / 2;
          Coord x2 = i->pv->X + (i->pv->Thickness + 1 + Bloat) / 2;
          Coord y1 = i->pv->Y - (i->pv->Thickness + 1 + Bloat) / 2;
          Coord y2 = i->pv->Y + (i->pv->Thickness + 1 + Bloat) / 2;
          if (IsRectangleInPolygon (x1, y1, x2, y2, polygon)
              && ADD_POLYGON_TO_LIST (i->layer, polygon, i->flag))
            longjmp (i->env, 1);
        }
      else if (TEST_FLAG (OCTAGONFLAG, i->pv))
        {
          POLYAREA *oct = OctagonPoly (i->pv->X, i->pv->Y, i->pv->Thickness / 2);
          if (isects (oct, polygon, true)
              && ADD_POLYGON_TO_LIST (i->layer, polygon, i->flag))
            longjmp (i->env, 1);
        }
      else if (IsPointInPolygon (i->pv->X, i->pv->Y, wide,
                                 polygon)
               && ADD_POLYGON_TO_LIST (i->layer, polygon, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

/*!
 * \brief Checks if a PV is connected to LOs, if it is, the LO is added
 * to the appropriate list and the 'used' flag is set.
 */
static bool
LookupLOConnectionsToPVList (int flag, bool AndRats)
{
  Cardinal layer_no;
  struct pv_info info;

  info.flag = flag;

  /* loop over all PVs currently on list */
  while (PVList.Location < PVList.Number)
    {
      BoxType search_box;

      /* get pointer to data */
      info.pv = PVLIST_ENTRY (PVList.Location);
      search_box = expand_bounds (&info.pv->BoundingBox);

      /* check pads */
      if (setjmp (info.env) == 0)
        r_search (PCB->Data->pad_tree, &search_box, NULL,
                  LOCtoPVpad_callback, &info);
      else
        return true;

      /* now all lines, arcs and polygons of the several layers */
      for (layer_no = 0; layer_no < max_copper_layer; layer_no++)
        {
          LayerType *layer = LAYER_PTR (layer_no);

          if (layer->no_drc)
             continue;

          info.layer = layer_no;

          /* add touching lines */
          if (setjmp (info.env) == 0)
            r_search (layer->line_tree, &search_box,
                      NULL, LOCtoPVline_callback, &info);
          else
            return true;
          /* add touching arcs */
          if (setjmp (info.env) == 0)
            r_search (layer->arc_tree, &search_box,
                      NULL, LOCtoPVarc_callback, &info);
          else
            return true;
          /* check all polygons */
          if (setjmp (info.env) == 0)
            r_search (layer->polygon_tree, &search_box,
                      NULL, LOCtoPVpoly_callback, &info);
          else
            return true;
        }
      /* Check for rat-lines that may intersect the PV */
      if (AndRats)
        {
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->rat_tree, &search_box, NULL,
                      LOCtoPVrat_callback, &info);
          else
            return true;
        }
      PVList.Location++;
    }
  return false;
}

/*!
 * \brief Find all connections between LO at the current list position
 * and new LOs.
 */
static bool
LookupLOConnectionsToLOList (int flag, bool AndRats)
{
  bool done;
  Cardinal i, group, layer, ratposition,
    lineposition[MAX_LAYER],
    polyposition[MAX_LAYER], arcposition[MAX_LAYER], padposition[2];

  /* copy the current LO list positions; the original data is changed
   * by 'LookupPVConnectionsToLOList()' which has to check the same
   * list entries plus the new ones
   */
  for (i = 0; i < max_copper_layer; i++)
    {
      lineposition[i] = LineList[i].Location;
      polyposition[i] = PolygonList[i].Location;
      arcposition[i]  = ArcList[i].Location;
    }
  for (i = 0; i < 2; i++)
    padposition[i] = PadList[i].Location;
  ratposition = RatList.Location;

  /* loop over all new LOs in the list; recurse until no
   * more new connections in the layergroup were found
   */
  do
    {
      Cardinal *position;

      if (AndRats)
        {
          position = &ratposition;
          for (; *position < RatList.Number; (*position)++)
            {
              group = RATLIST_ENTRY (*position)->group1;
              if (LookupLOConnectionsToRatEnd
                  (&(RATLIST_ENTRY (*position)->Point1), group, flag))
                return (true);
              group = RATLIST_ENTRY (*position)->group2;
              if (LookupLOConnectionsToRatEnd
                  (&(RATLIST_ENTRY (*position)->Point2), group, flag))
                return (true);
            }
        }
      /* loop over all layergroups */
      for (group = 0; group < max_group; group++)
        {
          Cardinal entry;

          for (entry = 0; entry < PCB->LayerGroups.Number[group]; entry++)
            {
              layer = PCB->LayerGroups.Entries[group][entry];

              /* be aware that the layer number equal max_copper_layer
               * and max_copper_layer+1 have a special meaning for pads
               */
              if (layer < max_copper_layer)
                {
                  /* try all new lines */
                  position = &lineposition[layer];
                  for (; *position < LineList[layer].Number; (*position)++)
                    if (LookupLOConnectionsToLine
                        (LINELIST_ENTRY (layer, *position), group, flag, true, AndRats))
                      return (true);

                  /* try all new arcs */
                  position = &arcposition[layer];
                  for (; *position < ArcList[layer].Number; (*position)++)
                    if (LookupLOConnectionsToArc
                        (ARCLIST_ENTRY (layer, *position), group, flag, AndRats))
                      return (true);

                  /* try all new polygons */
                  position = &polyposition[layer];
                  for (; *position < PolygonList[layer].Number; (*position)++)
                    if (LookupLOConnectionsToPolygon
                        (POLYGONLIST_ENTRY (layer, *position), group, flag, AndRats))
                      return (true);
                }
              else
                {
                  /* try all new pads */
                  layer -= max_copper_layer;
                  if (layer > 1)
                    {
                      Message (_("bad layer number %d max_copper_layer=%d in find.c\n"),
                               layer, max_copper_layer);
                      return false;
                    }
                  position = &padposition[layer];
                  for (; *position < PadList[layer].Number; (*position)++) {
                    if (LookupLOConnectionsToPad
                        (PADLIST_ENTRY (layer, *position), group, flag, AndRats))
                      return (true);
                  }
                }
            }
        }

      /* check if all lists are done; Later for-loops
       * may have changed the prior lists
       */
      done = !AndRats || ratposition >= RatList.Number;
      done = done && padposition[0] >= PadList[0].Number &&
                     padposition[1] >= PadList[1].Number;
      for (layer = 0; layer < max_copper_layer; layer++)
        done = done &&
               lineposition[layer] >= LineList[layer].Number &&
               arcposition[layer]  >= ArcList[layer].Number &&
               polyposition[layer] >= PolygonList[layer].Number;
    }
  while (!done);
  return (false);
}

static int
pv_pv_callback (const BoxType * b, void *cl)
{
  PinType *pin = (PinType *) b;
  struct pv_info *i = (struct pv_info *) cl;

  if (!TEST_FLAG (i->flag, pin) && PV_TOUCH_PV (i->pv, pin, &pimri))
    {
      if (TEST_FLAG (HOLEFLAG, pin) || TEST_FLAG (HOLEFLAG, i->pv))
        {
          SET_FLAG (WARNFLAG, pin);
          Settings.RatWarn = true;
          if (pin->Element)
            Message (_("WARNING: Hole too close to pin.\n"));
          else
            Message (_("WARNING: Hole too close to via.\n"));
        }
      else if (ADD_PV_TO_LIST (pin, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

/*!
 * \brief Searches for new PVs that are connected to PVs on the list.
 */
static bool
LookupPVConnectionsToPVList (int flag)
{
  Cardinal save_place;
  struct pv_info info;

  info.flag = flag;

  /* loop over all PVs on list */
  save_place = PVList.Location;
  while (PVList.Location < PVList.Number)
    {
      BoxType search_box;

      /* get pointer to data */
      info.pv = PVLIST_ENTRY (PVList.Location);
      search_box = expand_bounds ((BoxType *)info.pv);

      if (setjmp (info.env) == 0)
        r_search (PCB->Data->via_tree, &search_box, NULL,
                  pv_pv_callback, &info);
      else
        return true;
      if (setjmp (info.env) == 0)
        r_search (PCB->Data->pin_tree, &search_box, NULL,
                  pv_pv_callback, &info);
      else
        return true;
      PVList.Location++;
    }
  PVList.Location = save_place;
  return (false);
}

struct lo_info
{
  Cardinal layer;
  LineType *line;
  PadType *pad;
  ArcType *arc;
  PolygonType *polygon;
  RatType *rat;
  int flag;
  jmp_buf env;
};

static int
pv_line_callback (const BoxType * b, void *cl)
{
  PinType *pv = (PinType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, pv) && PinLineIntersect (pv, i->line, &pimri)) {
    if (TEST_FLAG (HOLEFLAG, pv)) {
      SET_FLAG (WARNFLAG, pv);
      Settings.RatWarn = true;
      Message (_("WARNING: Hole too close to line.\n"));
    }
    else if (ADD_PV_TO_LIST (pv, i->flag)) {
      longjmp (i->env, 1);
    }
  }
  return 0;
}

static int
pv_pad_callback (const BoxType * b, void *cl)
{
  PinType *pv = (PinType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, pv) && IS_PV_ON_PAD (pv, i->pad, &pimri))
    {
      if (TEST_FLAG (HOLEFLAG, pv))
        {
          SET_FLAG (WARNFLAG, pv);
          Settings.RatWarn = true;
          Message (_("WARNING: Hole too close to pad.\n"));
        }
      else if (ADD_PV_TO_LIST (pv, i->flag))
        {
          longjmp (i->env, 1);
        }
    }
  return 0;
}

static int
pv_arc_callback (const BoxType * b, void *cl)
{
  PinType *pv = (PinType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, pv) && IS_PV_ON_ARC (pv, i->arc, &pimri))
    {
      if (TEST_FLAG (HOLEFLAG, pv))
        {
          SET_FLAG (WARNFLAG, pv);
          Settings.RatWarn = true;
          Message (_("WARNING: Hole touches arc.\n"));
        }
      else if (ADD_PV_TO_LIST (pv, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
pv_poly_callback (const BoxType * b, void *cl)
{
  PinType *pv = (PinType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  /* note that holes in polygons are ok, so they don't generate warnings. */
  if (!TEST_FLAG (i->flag, pv) && !TEST_FLAG (HOLEFLAG, pv) &&
                                  (TEST_THERM (i->layer, pv) ||
                                   !TEST_FLAG (CLEARPOLYFLAG, i->polygon) ||
                                   !pv->Clearance))
    {
      if (TEST_FLAG (SQUAREFLAG, pv))
        {
          Coord x1, x2, y1, y2;
          x1 = pv->X - (PIN_SIZE (pv) + 1 + Bloat) / 2;
          x2 = pv->X + (PIN_SIZE (pv) + 1 + Bloat) / 2;
          y1 = pv->Y - (PIN_SIZE (pv) + 1 + Bloat) / 2;
          y2 = pv->Y + (PIN_SIZE (pv) + 1 + Bloat) / 2;
          if (IsRectangleInPolygon (x1, y1, x2, y2, i->polygon)
              && ADD_PV_TO_LIST (pv, i->flag))
            longjmp (i->env, 1);
        }
      else if (TEST_FLAG (OCTAGONFLAG, pv))
        {
          POLYAREA *oct = OctagonPoly (pv->X, pv->Y, PIN_SIZE (pv) / 2);
          if (isects (oct, i->polygon, true) && ADD_PV_TO_LIST (pv, i->flag))
            longjmp (i->env, 1);
        }
      else
        {
          if (IsPointInPolygon
              (pv->X, pv->Y, PIN_SIZE (pv) * 0.5 + Bloat, i->polygon)
              && ADD_PV_TO_LIST (pv, i->flag))
            longjmp (i->env, 1);
        }
    }
  return 0;
}

static int
pv_rat_callback (const BoxType * b, void *cl)
{
  PinType *pv = (PinType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  /* rats can't cause DRC so there is no early exit */
  if (!TEST_FLAG (i->flag, pv) && IS_PV_ON_RAT (pv, i->rat))
    ADD_PV_TO_LIST (pv, i->flag);
  return 0;
}

/*!
 * \brief Searches for new PVs that are connected to NEW LOs on the list.
 *
 * This routine updates the position counter of the lists too.
 */
static bool
LookupPVConnectionsToLOList (int flag, bool AndRats)
{
  Cardinal layer_no;
  struct lo_info info;

  info.flag = flag;

  /* loop over all layers */
  for (layer_no = 0; layer_no < max_copper_layer; layer_no++)
    {
      LayerType *layer = LAYER_PTR (layer_no);

      if (layer->no_drc)
                       continue;
      /* do nothing if there are no PV's */
      if (TotalP + TotalV == 0)
        {
          LineList[layer_no].Location = LineList[layer_no].Number;
          ArcList[layer_no].Location = ArcList[layer_no].Number;
          PolygonList[layer_no].Location = PolygonList[layer_no].Number;
          continue;
        }

      /* check all lines */
      while (LineList[layer_no].Location < LineList[layer_no].Number)
        {
          BoxType search_box;

          info.line = LINELIST_ENTRY (layer_no, LineList[layer_no].Location);
          search_box = expand_bounds ((BoxType *)info.line);

          if (setjmp (info.env) == 0)
            r_search (PCB->Data->via_tree, &search_box, NULL,
                      pv_line_callback, &info);
          else
            return true;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pin_tree, &search_box, NULL,
                      pv_line_callback, &info);
          else
            return true;
          LineList[layer_no].Location++;
        }

      /* check all arcs */
      while (ArcList[layer_no].Location < ArcList[layer_no].Number)
        {
          BoxType search_box;

          info.arc = ARCLIST_ENTRY (layer_no, ArcList[layer_no].Location);
          search_box = expand_bounds ((BoxType *)info.arc);

          if (setjmp (info.env) == 0)
            r_search (PCB->Data->via_tree, &search_box, NULL,
                      pv_arc_callback, &info);
          else
            return true;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pin_tree, &search_box, NULL,
                      pv_arc_callback, &info);
          else
            return true;
          ArcList[layer_no].Location++;
        }

      /* now all polygons */
      info.layer = layer_no;
      while (PolygonList[layer_no].Location < PolygonList[layer_no].Number)
        {
          BoxType search_box;

          info.polygon = POLYGONLIST_ENTRY (layer_no, PolygonList[layer_no].Location);
          search_box = expand_bounds ((BoxType *)info.polygon);

          if (setjmp (info.env) == 0)
            r_search (PCB->Data->via_tree, &search_box, NULL,
                      pv_poly_callback, &info);
          else
            return true;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pin_tree, &search_box, NULL,
                      pv_poly_callback, &info);
          else
            return true;
          PolygonList[layer_no].Location++;
        }
    }

  /* loop over all pad-layers */
  for (layer_no = 0; layer_no < 2; layer_no++)
    {
      /* do nothing if there are no PV's */
      if (TotalP + TotalV == 0)
        {
          PadList[layer_no].Location = PadList[layer_no].Number;
          continue;
        }

      /* check all pads; for a detailed description see
       * the handling of lines in this subroutine
       */
      while (PadList[layer_no].Location < PadList[layer_no].Number)
        {
          BoxType search_box;

          info.pad = PADLIST_ENTRY (layer_no, PadList[layer_no].Location);
          search_box = expand_bounds ((BoxType *)info.pad);

          if (setjmp (info.env) == 0)
            r_search (PCB->Data->via_tree, &search_box, NULL,
                      pv_pad_callback, &info);
          else
            return true;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pin_tree, &search_box, NULL,
                      pv_pad_callback, &info);
          else
            return true;
          PadList[layer_no].Location++;
        }
    }

  /* do nothing if there are no PV's */
  if (TotalP + TotalV == 0)
    RatList.Location = RatList.Number;

  /* check all rat-lines */
  if (AndRats)
    {
      while (RatList.Location < RatList.Number)
        {
          info.rat = RATLIST_ENTRY (RatList.Location);
          r_search_pt (PCB->Data->via_tree, & info.rat->Point1, 1, NULL,
                    pv_rat_callback, &info);
          r_search_pt (PCB->Data->via_tree, & info.rat->Point2, 1, NULL,
                    pv_rat_callback, &info);
          r_search_pt (PCB->Data->pin_tree, & info.rat->Point1, 1, NULL,
                    pv_rat_callback, &info);
          r_search_pt (PCB->Data->pin_tree, & info.rat->Point2, 1, NULL,
                    pv_rat_callback, &info);

          RatList.Location++;
        }
    }
  return (false);
}

/* Shorthand macro for conditional copy of some stuff between types */
#define SET_XY_IF_NOT_NULL(target, source) \
  do {                                     \
    if ( target != NULL ) {                \
      (target)->X = (source).x;            \
      (target)->Y = (source).y;            \
    }                                      \
  } while ( 0 )

static bool
ArcArcIntersect (ArcType *Arc1, ArcType *Arc2, PointType *pii)
{
  /* We're going to do this by detecting any intersections of the inner
   * or outer edges of Arc1 with the inner or outer edges of Arc2 or with
   * the end caps of Arc2, and vice versa.  Note that this doesn't catch
   * cases where Arc1 is entirely contained withing Arc2 or vice versa,
   * but we don't care because in that situation the contained arc can't
   * change the overall connectivity anyway.  This is true (as required)
   * even under non-zero bloat, since bloat is linear and therefore doesn't
   * change containment.  For the (round) end caps area detection is used,
   * which takes care of the potential problem of "chinks" at corners into
   * which otheer edges might fall: they are plugged by the area end caps.
   * The alternative of using area detection everywhere is a little more
   * painful and can't be factored as easily into the terms of existing
   * code.  */

  /* We don't handle arc of ellipse at the moment */
  assert (Arc1->Width == Arc1->Height);
  assert (Arc2->Width == Arc2->Height);
  
  /* We don't handle arcs with square ends */
  assert (! TEST_FLAG (SQUAREFLAG, Arc1));
  assert (! TEST_FLAG (SQUAREFLAG, Arc2));

  Coord a1to2 = Bloat / 2 + Arc1->Thickness / 2;      /* Arc1 Thickness / 2 */
  Coord a2to2 = Bloat / 2 + Arc2->Thickness / 2;      /* Arc2 Thickness / 2 */
  
  /* If either arc's thickness has reached 0 at the current Bloat, then
   * we're done.  In painful theory there could still be intersections at 0
   * thickness, and since we're on integer coordinates they could in some
   * situation be detected, but not consistently because the underlying
   * calculatios use floating point, making consistant tangent intersection
   * detection impossible. */
  if ( a1to2 <= 0 || a2to2 <= 0 ) {
    return false;
  }

  /* Note that Bloat doesn't change radius.  */
  Coord rad1 = Arc1->Width, rad2 = Arc2->Width;
  
  /* Convert the arc angles to the conventions used in geometry.h */
  Angle a1sa, a1ad;   /* Arc1 Start Angle/Angle Delta */
  pcb_to_geometry_angle_range (Arc1->StartAngle, Arc1->Delta, &a1sa, &a1ad);
  Angle a2sa, a2ad;   /* Arc1 Start Angle/Angle Delta */
  pcb_to_geometry_angle_range (Arc2->StartAngle, Arc2->Delta, &a2sa, &a2ad);

  /* These arcs go down the middle of the fat "arcs" Arc1 and Arc2 */
  Arc
    a1 = { { { Arc1->X, Arc1->Y }, rad1 }, a1sa, a1ad },
    a2 = { { { Arc2->X, Arc2->Y }, rad2 }, a2sa, a2ad };
    
  /* If we have arcs of identical circles just check if the arc center
   * lines overlap.  The arc_arc_intersection() function doesn't try to
   * return the details of the intersection in this case, so we can't use
   * it.  */
  if ( arc_arc_intersection (&a1, &a2, NULL) == INT_MAX ) {

    Angle osa, oad;   /* Overlap Start Angle, Overlap Angle Delta */
    /* Angular Spans Overlap */
    bool aso = angular_spans_overlap (a1sa, a1ad, a2sa, a2ad, &osa, &oad);
    if ( aso ) {
      if ( pii != NULL ) {
        Angle aocoo = osa + (oad / 2.0);   /* Angle Of Center Of Overlap */
        /* Point In Intersection As Point (not yet PointType :) */
        Point piiap = {
          a1.circle.center.x + a1.circle.radius * cos (aocoo),   
          a1.circle.center.y + a1.circle.radius * sin (aocoo) };
        pii->X = piiap.x;
        pii->Y = piiap.y;
      }
      return true;
    }
    else {
      return false;
    }
  }
  
  /* Inner/Outer Arcs (of ArcType Arc, due to its thickness).  Note that the
   * inner arcs might have radius <= 0 even at Bloat == 0, but the outer
   * arcs should never have radius <= 0 at this point since we've already
   * returned false for thickness of 0 or less.  */
  Arc
    a1oa = { { a1.circle.center, rad1 + a1to2 }, a1sa, a1ad },
    a1ia = { { a1.circle.center, rad1 - a1to2 }, a1sa, a1ad },
    a2oa = { { a2.circle.center, rad2 + a2to2 }, a2sa, a2ad },
    a2ia = { { a2.circle.center, rad2 - a2to2 }, a2sa, a2ad };
  assert (a1oa.circle.radius > 0);
  assert (a2oa.circle.radius > 0);

  Point intersection[2];

  /* Check if any of the arc edges intersect, skipping degenerate edges */
  if ( arc_arc_intersection (&a1oa, &a2oa, intersection) ) {
    SET_XY_IF_NOT_NULL (pii, intersection[0]);
    return TRUE;
  } 
  if ( a1ia.circle.radius > 0 ) {
    if ( arc_arc_intersection (&a1ia, &a2oa, intersection) ) {
      SET_XY_IF_NOT_NULL (pii, intersection[0]);
      return TRUE;
    } 
  }
  if ( a2ia.circle.radius > 0 ) {
    if ( arc_arc_intersection (&a1oa, &a2ia, intersection) ) {
      SET_XY_IF_NOT_NULL (pii, intersection[0]);
      return TRUE;
    } 
  }
  if ( a1ia.circle.radius > 0 && a2ia.circle.radius > 0 ) {
    if ( arc_arc_intersection (&a1ia, &a2ia, intersection) ) {
      SET_XY_IF_NOT_NULL (pii, intersection[0]);
      return TRUE;
    } 
  }

  /* Check if any of the end caps touch other arc (including it's end caps) */
  {
    Point a1ep[2], a2ep[2];   /* Arc 1/2 End Points */
    arc_end_points (&a1, a1ep);
    arc_end_points (&a2, a2ep);
    Point np;                 /* Nearest Point (reused) */
    Circle epc, apc;          /* End/Arc Point Circle (reused) */
    Point piiap;              /* Point In Intersection As Point */

    np = nearest_point_on_arc (a1ep[0], &a2);
    epc = (Circle) { a1ep[0], a1to2 }; 
    apc = (Circle) { np, a2to2 };
    if ( circle_intersects_circle (&epc, &apc, &piiap) ) {
      SET_XY_IF_NOT_NULL (pii, piiap);
      return TRUE;
    }

    np = nearest_point_on_arc (a1ep[1], &a2);
    epc = (Circle) { a1ep[1], a1to2 }; 
    apc = (Circle) { np, a2to2 };
    if ( circle_intersects_circle (&epc, &apc, &piiap) ) {
      SET_XY_IF_NOT_NULL (pii, piiap);
      return TRUE;
    }

    np = nearest_point_on_arc (a2ep[0], &a1);
    epc = (Circle) { a2ep[0], a2to2 }; 
    apc = (Circle) { np, a1to2 };
    if ( circle_intersects_circle (&epc, &apc, &piiap) ) {
      SET_XY_IF_NOT_NULL (pii, piiap);
      return TRUE;
    }
    
    np = nearest_point_on_arc (a2ep[1], &a1);
    epc = (Circle) { a2ep[1], a2to2 }; 
    apc = (Circle) { np, a1to2 };
    if ( circle_intersects_circle (&epc, &apc, &piiap) ) {
      SET_XY_IF_NOT_NULL (pii, piiap);
      return TRUE;
    }
  }

  return FALSE;
}

/*!
 * \brief Tests if point is same as line end point.
 */
static bool
IsRatPointOnLineEnd (PointType *Point, LineType *Line)
{
  if ((Point->X == Line->Point1.X
       && Point->Y == Line->Point1.Y)
      || (Point->X == Line->Point2.X && Point->Y == Line->Point2.Y))
    return (true);
  return (false);
}

/*!
 * \brief Writes vertices of a squared line.
 */
static void 
form_slanted_rectangle (PointType p[4], LineType *l)
{
   double dwx = 0, dwy = 0;
   if (l->Point1.Y == l->Point2.Y)
     dwx = l->Thickness / 2.0;
   else if (l->Point1.X == l->Point2.X)
     dwy = l->Thickness / 2.0;
   else 
     {
       Coord dX = l->Point2.X - l->Point1.X;
       Coord dY = l->Point2.Y - l->Point1.Y;
       double r = Distance (l->Point1.X, l->Point1.Y, l->Point2.X, l->Point2.Y);
       dwx = l->Thickness / 2.0 / r * dX;
       dwy = l->Thickness / 2.0 / r * dY;
     }
    p[0].X = l->Point1.X - dwx + dwy; p[0].Y = l->Point1.Y - dwy - dwx;
    p[1].X = l->Point1.X - dwx - dwy; p[1].Y = l->Point1.Y - dwy + dwx;
    p[2].X = l->Point2.X + dwx - dwy; p[2].Y = l->Point2.Y + dwy + dwx;
    p[3].X = l->Point2.X + dwx + dwy; p[3].Y = l->Point2.Y + dwy - dwx;
}

/*!
 * \brief Checks if two lines intersect, and also return a point in the
 * intersection if the pii argument is non-NULL.
 *
 * <pre>
 * From news FAQ:
 *
 * Let A,B,C,D be 2-space position vectors.
 *
 * Then the directed line segments AB & CD are given by:
 *
 *      AB=A+r(B-A), r in [0,1]
 *
 *      CD=C+s(D-C), s in [0,1]
 *
 * If AB & CD intersect, then
 *
 *      A+r(B-A)=C+s(D-C), or
 *
 *      XA+r(XB-XA)=XC+s*(XD-XC)
 *
 *      YA+r(YB-YA)=YC+s(YD-YC)  for some r,s in [0,1]
 *
 * Solving the above for r and s yields
 *
 *          (YA-YC)(XD-XC)-(XA-XC)(YD-YC)
 *      r = -----------------------------  (eqn 1)
 *          (XB-XA)(YD-YC)-(YB-YA)(XD-XC)
 *
 *          (YA-YC)(XB-XA)-(XA-XC)(YB-YA)
 *      s = -----------------------------  (eqn 2)
 *          (XB-XA)(YD-YC)-(YB-YA)(XD-XC)
 *
 * Let I be the position vector of the intersection point, then:
 *
 *      I=A+r(B-A) or
 *
 *      XI=XA+r(XB-XA)
 *
 *      YI=YA+r(YB-YA)
 *
 * By examining the values of r & s, you can also determine some
 * other limiting conditions:
 *
 *      If 0<=r<=1 & 0<=s<=1, intersection exists
 *
 *          r<0 or r>1 or s<0 or s>1 line segments do not intersect
 *
 *      If the denominator in eqn 1 is zero, AB & CD are parallel
 *
 *      If the numerator in eqn 1 is also zero, AB & CD are coincident
 *
 * If the intersection point of the 2 lines are needed (lines in this
 * context mean infinite lines) regardless whether the two line
 * segments intersect, then
 *
 *      If r>1, I is located on extension of AB
 *      If r<0, I is located on extension of BA
 *      If s>1, I is located on extension of CD
 *      If s<0, I is located on extension of DC
 *
 * Also note that the denominators of eqn 1 & 2 are identical.
 * </pre>
 */
bool
LineLineIntersect (LineType *Line1, LineType *Line2, PointType *pii)
{
  double s, r;
  double line1_dx, line1_dy, line2_dx, line2_dy,
         point1_dx, point1_dy;
  if (TEST_FLAG (SQUAREFLAG, Line1))/* pretty reckless recursion */
    {
      PointType p[4];
      form_slanted_rectangle (p, Line1);
      return IsLineInQuadrangle (p, Line2, pii);
    }
  /* here come only round Line1 because IsLineInQuadrangle()
     calls LineLineIntersect() with first argument rounded*/
  if (TEST_FLAG (SQUAREFLAG, Line2))
    {
      PointType p[4];
      form_slanted_rectangle (p, Line2);
      return IsLineInQuadrangle (p, Line1, pii);
    }
  /* now all lines are round */

  /* Check endpoints: this provides a quick exit, catches
   * cases where the "real" lines don't intersect but the
   * thick lines touch, and ensures that the dx/dy business
   * below does not cause a divide-by-zero. */
  if (    IsPointInPad (Line2->Point1.X, Line2->Point1.Y,
                        MAX (Line2->Thickness / 2 + Bloat, 0),
                        (PadType *) Line1, pii)
       || IsPointInPad (Line2->Point2.X, Line2->Point2.Y,
                        MAX (Line2->Thickness / 2 + Bloat, 0),
                        (PadType *) Line1, pii)
       || IsPointInPad (Line1->Point1.X, Line1->Point1.Y,
                        MAX (Line1->Thickness / 2 + Bloat, 0),
                        (PadType *) Line2, pii)
       || IsPointInPad (Line1->Point2.X, Line1->Point2.Y,
                        MAX (Line1->Thickness / 2 + Bloat, 0),
                        (PadType *) Line2, pii) )
    return true;

  /* setup some constants */
  line1_dx = Line1->Point2.X - Line1->Point1.X;
  line1_dy = Line1->Point2.Y - Line1->Point1.Y;
  line2_dx = Line2->Point2.X - Line2->Point1.X;
  line2_dy = Line2->Point2.Y - Line2->Point1.Y;
  point1_dx = Line1->Point1.X - Line2->Point1.X;
  point1_dy = Line1->Point1.Y - Line2->Point1.Y;

  /* If either line is a point, we have failed already, since the
   *   endpoint check above will have caught an "intersection". */
  if ((line1_dx == 0 && line1_dy == 0)
      || (line2_dx == 0 && line2_dy == 0))
    return false;

  /* set s to cross product of Line1 and the line
   *   Line1.Point1--Line2.Point1 (as vectors) */
  s = point1_dy * line1_dx - point1_dx * line1_dy;

  /* set r to cross product of both lines (as vectors) */
  r = line1_dx * line2_dy - line1_dy * line2_dx;

  /* No cross product means parallel lines, or maybe Line2 is
   *  zero-length. In either case, since we did a bounding-box
   *  check before getting here, the above IsPointInPad() checks
   *  will have caught any intersections. */
  if (r == 0.0)
    return false;

  s /= r;
  r = (point1_dy * line2_dx - point1_dx * line2_dy) / r;

  /* intersection is at least on AB */
  if (r >= 0.0 && r <= 1.0) {
    if ( s >= 0.0 && s <= 1.0 ) {
      if ( pii != NULL ) {
        pii->X = Line1->Point1.X + r * line1_dx;
        pii->Y = Line1->Point1.Y + r * line1_dy;
      }
      return true;
    }
    return false;
  }

  /* intersection is at least on CD */
  /* [removed this case since it always returns false --asp] */
  return false;
}

/*!
 * \brief Check for line intersection with an arc.
 *
 * There are a lot of cases to consider due to square/round line end caps and
 * arcs with thickness / 2 > radius, so almost a full constructive geometry
 * solution is used, with checks between most combinations of sub-figures.
 */ 
bool
LineArcIntersect (LineType *Line, ArcType *arc, PointType *pii)
{
  /* We don't handle arc of ellipse at the moment */
  assert (arc->Width == arc->Height);
 
  /* Note that Bloat doesn't change radius (arcs are shrunk "in place").  */
  Coord radius = arc->Width;
  
  /* We don't handle arcs with square ends */
  assert (! TEST_FLAG (SQUAREFLAG, arc));

  Coord lto2 = Bloat / 2 + Line->Thickness / 2;       /* Line Thickness / 2 */
  Coord ato2 = Bloat / 2 + arc->Thickness / 2;        /* Arc Thickness / 2 */
  /* Both thickness / 2 (Sum of Thicknesses Over 2) */
  Coord sto2 = Bloat + (Line->Thickness + arc->Thickness) / 2;
  
  /* If either the arc or line thickness has reached 0 at the current Bloat,
   * then we're done.  In painful theory of course there could still be
   * intersections at 0 thickness, and since we're on integer coordinates
   * they could in some situation be detected, but not consistently because
   * the underlying calculatios use floating point.  */
  if ( lto2 <= 0 || ato2 <= 0 ) {
    return false;
  }

  /* Rectangular Part Of Line */
  Rectangle rpol = rectangular_part_of_line (Line, Bloat / 2);

  LineSegment rectangle_edges[4] = {
    ((LineSegment) { rpol.corner[0], rpol.corner[1] }),
    ((LineSegment) { rpol.corner[1], rpol.corner[2] }),
    ((LineSegment) { rpol.corner[2], rpol.corner[3] }),
    ((LineSegment) { rpol.corner[3], rpol.corner[0] }) };
  
  /* Convert the arc angles to the conventions used in geometry.h */
  Angle sa, ad;   /* Start Angle, Angle Delta */
  pcb_to_geometry_angle_range (arc->StartAngle, arc->Delta, &sa, &ad);

  /* Inner/Outer Arcs (of ArcType Arc, due to its thickness).  Note that
   * ia might have radius <= 0 even at Bloat == 0, but oa should never
   * have radius <= 0 at this point since we've already returned false for
   * thickness of 0 or less above.  */
  Arc
    oa = { { { arc->X, arc->Y }, radius + ato2 }, sa, ad },
    ia = { { { arc->X, arc->Y }, radius - ato2 }, sa, ad };
  assert (oa.circle.radius > 0);

  Arc acl = { { { arc->X, arc->Y}, radius }, sa, ad };   /* Arc Center Line */
  Point aep[2];                                          /* Arc End Points */
  arc_end_points (&acl, aep);
 
  /* Check if the rectangular part of line intersects anything */
  for ( int ii = 0 ; ii < 4 ; ii++ ) {

    /* Check arc edges */
    Point ip[2];   /* Intersection Points */
    int ipc;       /* Intersection Point Count */
    ipc = arc_line_segment_intersection (&oa, &(rectangle_edges[ii]), ip);
    if ( ipc > 0 ) {
      SET_XY_IF_NOT_NULL (pii, ip[0]);
      return true;
    }
    if ( ia.circle.radius > 0 ) {   /* If ia isn't degenerate check it also */
      ipc = arc_line_segment_intersection (&ia, &(rectangle_edges[ii]), ip);
      if ( ipc > 0 ) {
        SET_XY_IF_NOT_NULL (pii, ip[0]);
        return true;
      }
    }

    /* Check arc end caps (which are always round).  Note: this catches the
     * case in which the line is entirely contained within an arc end cap.  */
    for ( int jj = 0 ; jj < 2 ; jj++ ) {
      Point npol
        = nearest_point_on_probably_axis_aligned_line_segment (
            aep[jj],
            &(rectangle_edges[ii]) );
      if ( vec_mag (vec_from (aep[jj], npol) ) <= ato2 ) {
        SET_XY_IF_NOT_NULL (pii, npol);
        return true;
      }
    }
    
    /* The rectangular line might be entirely contained between the inner and
     * outer edges of the arc.  In this case all the corners of the rectangle
     * will be within ato2 of acl, so it's sufficient to check one of them.  */
    Point ctc = rpol.corner[0];                   /* Corner To Check */
    Point nptc = nearest_point_on_arc (ctc, &acl);/* Nearest Point To Corner */
    if ( vec_mag (vec_from (ctc, nptc)) <= ato2 ) {
      SET_XY_IF_NOT_NULL (pii, ctc);
      return true;
    }
  }
  
  /* Unless the line has square end caps (in which case they will already
   * have been incorporated into the single rectangle used to represent
   * the line)... */
  if ( ! TEST_FLAG (SQUAREFLAG, Line) ) {

    Point leccs[2] = {   /* Line End Cap Centers */
      { Line->Point1.X, Line->Point1.Y },
      { Line->Point2.X, Line->Point2.Y } };
    
    /* Check if the line end caps intersect the arc.  Note that this catches
     * intersections with the arc end caps, since they are always round.  */
    for ( int ii = 0 ; ii < 2 ; ii++ ) {
      Point lecc = leccs[ii];   /* Line End Cap Center */
      Point np2lep;   /* Nearest Point to Line End Point */
      np2lep = nearest_point_on_arc (lecc, &acl);
      /* (Vector from) Line Cap Center to Nearest Point (on acl) */
      Vec lcc_np = vec_from (lecc, np2lep);
      if ( vec_mag (lcc_np) <= sto2 ) {
        if ( pii != NULL ) {
          Point piiap   /* Point In Intersection As Point */
            = vec_sum (lecc, vec_scale (lcc_np, (lto2 / vec_mag (lcc_np))));
          pii->X = piiap.x;
          pii->Y = piiap.y;
        }
        return true;
      } 
    }

  } 

  return false;
}

static int
LOCtoArcLine_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, line) && LineArcIntersect (line, i->arc, &pimri))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoArcArc_callback (const BoxType * b, void *cl)
{
  ArcType *arc = (ArcType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!arc->Thickness)
    return 0;
  if (!TEST_FLAG (i->flag, arc) && ArcArcIntersect (i->arc, arc, &pimri))
    {
      if (ADD_ARC_TO_LIST (i->layer, arc, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoArcPad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if ( !TEST_FLAG (i->flag, pad) &&
        i->layer == (TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE) &&
        ArcPadIntersect (i->arc, pad, &pimri) &&
        ADD_PAD_TO_LIST (i->layer, pad, i->flag) ) {
    longjmp (i->env, 1);
  }
  return 0;
}

/*!
 * \brief Searches all LOs that are connected to the given arc on the
 * given layergroup.
 *
 * All found connections are added to the list.
 *
 * The notation that is used is:\n
 * Xij means Xj at arc i.
 */
static bool
LookupLOConnectionsToArc (ArcType *Arc, Cardinal LayerGroup, int flag, bool AndRats)
{
  Cardinal entry;
  struct lo_info info;
  BoxType search_box;

  info.flag = flag;
  info.arc = Arc;
  search_box = expand_bounds ((BoxType *)info.arc);

  /* loop over all layers of the group */
  for (entry = 0; entry < PCB->LayerGroups.Number[LayerGroup]; entry++)
    {
      Cardinal layer_no;
      LayerType *layer;
      GList *i;

      layer_no = PCB->LayerGroups.Entries[LayerGroup][entry];
      layer = LAYER_PTR (layer_no);

      /* handle normal layers */
      if (layer_no < max_copper_layer)
        {
          info.layer = layer_no;
          /* add arcs */
          if (setjmp (info.env) == 0)
            r_search (layer->line_tree, &search_box,
                      NULL, LOCtoArcLine_callback, &info);
          else
            return true;

          if (setjmp (info.env) == 0)
            r_search (layer->arc_tree, &search_box,
                      NULL, LOCtoArcArc_callback, &info);
          else
            return true;

          /* now check all polygons */
          for (i = layer->Polygon; i != NULL; i = g_list_next (i))
            {
              PolygonType *polygon = i->data;
              if (!TEST_FLAG (flag, polygon) && IsArcInPolygon (Arc, polygon)
                  && ADD_POLYGON_TO_LIST (layer_no, polygon, flag))
                return true;
            }
        }
      else
        {
          info.layer = layer_no - max_copper_layer;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pad_tree, &search_box, NULL,
                      LOCtoArcPad_callback, &info);
          else
            return true;
        }
    }
  return (false);
}

static int
LOCtoLineLine_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, line) && LineLineIntersect (i->line, line, &pimri))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoLineArc_callback (const BoxType * b, void *cl)
{
  ArcType *arc = (ArcType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!arc->Thickness)
    return 0;
  if (!TEST_FLAG (i->flag, arc) && LineArcIntersect (i->line, arc, &pimri))
    {
      if (ADD_ARC_TO_LIST (i->layer, arc, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoLineRat_callback (const BoxType * b, void *cl)
{
  RatType *rat = (RatType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, rat))
    {
      if ((rat->group1 == i->layer)
          && IsRatPointOnLineEnd (&rat->Point1, i->line))
        {
          if (ADD_RAT_TO_LIST (rat, i->flag))
            longjmp (i->env, 1);
        }
      else if ((rat->group2 == i->layer)
               && IsRatPointOnLineEnd (&rat->Point2, i->line))
        {
          if (ADD_RAT_TO_LIST (rat, i->flag))
            longjmp (i->env, 1);
        }
    }
  return 0;
}

static int
LOCtoLinePad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if ( !TEST_FLAG (i->flag, pad) &&
       i->layer == (TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE) &&
       LinePadIntersect (i->line, pad, &pimri) &&
       ADD_PAD_TO_LIST (i->layer, pad, i->flag) ) {
    longjmp (i->env, 1);
  }
  return 0;
}

/*!
 * \brief Searches all LOs that are connected to the given line on the
 * given layergroup.
 *
 * All found connections are added to the list.
 *
 * The notation that is used is:
 * Xij means Xj at line i.
 */
static bool
LookupLOConnectionsToLine (LineType *Line, Cardinal LayerGroup,
                           int flag, bool PolysTo, bool AndRats)
{
  Cardinal entry;
  struct lo_info info;
  BoxType search_box;

  info.flag = flag;
  info.layer = LayerGroup;
  info.line = Line;
  search_box = expand_bounds ((BoxType *)info.line);

  if (AndRats)
    {
      /* add the new rat lines */
      if (setjmp (info.env) == 0)
        r_search (PCB->Data->rat_tree, &search_box, NULL,
                  LOCtoLineRat_callback, &info);
      else
        return true;
    }

  /* loop over all layers of the group */
  for (entry = 0; entry < PCB->LayerGroups.Number[LayerGroup]; entry++)
    {
      Cardinal layer_no;
      LayerType *layer;

      layer_no = PCB->LayerGroups.Entries[LayerGroup][entry];
      layer = LAYER_PTR (layer_no);

      /* handle normal layers */
      if (layer_no < max_copper_layer)
        {
          info.layer = layer_no;
          /* add lines */
          if (setjmp (info.env) == 0)
            r_search (layer->line_tree, &search_box,
                      NULL, LOCtoLineLine_callback, &info);
          else
            return true;
          /* add arcs */
          if (setjmp (info.env) == 0)
            r_search (layer->arc_tree, &search_box,
                      NULL, LOCtoLineArc_callback, &info);
          else
            return true;
          /* now check all polygons */
          if (PolysTo)
            {
              GList *i;
              for (i = layer->Polygon; i != NULL; i = g_list_next (i))
                {
                  PolygonType *polygon = i->data;
                  if (!TEST_FLAG (flag, polygon) && IsLineInPolygon (Line, polygon)
                      && ADD_POLYGON_TO_LIST (layer_no, polygon, flag))
                    return true;
                }
            }
        }
      else
        {
          /* handle special 'pad' layers */
          info.layer = layer_no - max_copper_layer;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pad_tree, &search_box, NULL,
                      LOCtoLinePad_callback, &info);
          else
            return true;
        }
    }
  return (false);
}

struct rat_info
{
  Cardinal layer;
  PointType *Point;
  int flag;
  jmp_buf env;
};

static int
LOCtoRat_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct rat_info *i = (struct rat_info *) cl;

  if (!TEST_FLAG (i->flag, line) &&
      ((line->Point1.X == i->Point->X &&
        line->Point1.Y == i->Point->Y) ||
       (line->Point2.X == i->Point->X && line->Point2.Y == i->Point->Y)))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}
static int
PolygonToRat_callback (const BoxType * b, void *cl)
{
  PolygonType *polygon = (PolygonType *) b;
  struct rat_info *i = (struct rat_info *) cl;

  if (!TEST_FLAG (i->flag, polygon) && polygon->Clipped &&
      (i->Point->X == polygon->Clipped->contours->head.point[0]) &&
      (i->Point->Y == polygon->Clipped->contours->head.point[1]))
    {
      if (ADD_POLYGON_TO_LIST (i->layer, polygon, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct rat_info *i = (struct rat_info *) cl;

  /* Note: this one doesn't need to worry about setting pimri at the moment
   * because it's currently only called from LookupLOConnectionsToRatEnd()
   * (which we don't care about because violations shouldn't happen for rat
   * intersections themselves, though I think they can be produced when a
   * violation in a rat-connected set changes connectivity via something
   * real). */

  if (!TEST_FLAG (i->flag, pad) && i->layer ==
	(TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE) &&
      ((pad->Point1.X == i->Point->X && pad->Point1.Y == i->Point->Y) ||
       (pad->Point2.X == i->Point->X && pad->Point2.Y == i->Point->Y) ||
       ((pad->Point1.X + pad->Point2.X) / 2 == i->Point->X &&
        (pad->Point1.Y + pad->Point2.Y) / 2 == i->Point->Y)) &&
      ADD_PAD_TO_LIST (i->layer, pad, i->flag))
    longjmp (i->env, 1);
  return 0;
}

/*!
 * \brief Searches all LOs that are connected to the given rat-line on
 * the given layergroup.
 *
 * All found connections are added to the list.
 *
 * The notation that is used is:
 * Xij means Xj at line i.
 */
static bool
LookupLOConnectionsToRatEnd (PointType *Point, Cardinal LayerGroup, int flag)
{
  Cardinal entry;
  struct rat_info info;

  info.flag = flag;
  info.Point = Point;
  /* loop over all layers of this group */
  for (entry = 0; entry < PCB->LayerGroups.Number[LayerGroup]; entry++)
    {
      Cardinal layer_no;
      LayerType *layer;

      layer_no = PCB->LayerGroups.Entries[LayerGroup][entry];
      layer = LAYER_PTR (layer_no);
      /* handle normal layers 
         rats don't ever touch
         arcs by definition
       */

      if (layer_no < max_copper_layer)
        {
          info.layer = layer_no;
          if (setjmp (info.env) == 0)
            r_search_pt (layer->line_tree, Point, 1, NULL,
                      LOCtoRat_callback, &info);
          else
            return true;
          if (setjmp (info.env) == 0)
            r_search_pt (layer->polygon_tree, Point, 1,
                      NULL, PolygonToRat_callback, &info);
        }
      else
        {
          /* handle special 'pad' layers */
          info.layer = layer_no - max_copper_layer;
          if (setjmp (info.env) == 0)
            r_search_pt (PCB->Data->pad_tree, Point, 1, NULL,
                      LOCtoPad_callback, &info);
          else
            return true;
        }
    }
  return (false);
}

static int
LOCtoPadLine_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, line) && LinePadIntersect (line, i->pad, &pimri))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPadArc_callback (const BoxType * b, void *cl)
{
  ArcType *arc = (ArcType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!arc->Thickness)
    return 0;
  if (!TEST_FLAG (i->flag, arc) && ArcPadIntersect (arc, i->pad, &pimri))
    {
      if (ADD_ARC_TO_LIST (i->layer, arc, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPadPoly_callback (const BoxType * b, void *cl)
{
  PolygonType *polygon = (PolygonType *) b;
  struct lo_info *i = (struct lo_info *) cl;


  if (!TEST_FLAG (i->flag, polygon) &&
      (!TEST_FLAG (CLEARPOLYFLAG, polygon) || !i->pad->Clearance))
    {
      if (IsPadInPolygon (i->pad, polygon) &&
          ADD_POLYGON_TO_LIST (i->layer, polygon, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPadRat_callback (const BoxType * b, void *cl)
{
  RatType *rat = (RatType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, rat))
    {
      if (rat->group1 == i->layer &&
	  ((rat->Point1.X == i->pad->Point1.X && rat->Point1.Y == i->pad->Point1.Y) ||
	   (rat->Point1.X == i->pad->Point2.X && rat->Point1.Y == i->pad->Point2.Y) ||
	   (rat->Point1.X == (i->pad->Point1.X + i->pad->Point2.X) / 2 &&
	    rat->Point1.Y == (i->pad->Point1.Y + i->pad->Point2.Y) / 2)))
        {
          if (ADD_RAT_TO_LIST (rat, i->flag))
            longjmp (i->env, 1);
        }
      else if (rat->group2 == i->layer &&
	       ((rat->Point2.X == i->pad->Point1.X && rat->Point2.Y == i->pad->Point1.Y) ||
		(rat->Point2.X == i->pad->Point2.X && rat->Point2.Y == i->pad->Point2.Y) ||
		(rat->Point2.X == (i->pad->Point1.X + i->pad->Point2.X) / 2 &&
		 rat->Point2.Y == (i->pad->Point1.Y + i->pad->Point2.Y) / 2)))
        {
          if (ADD_RAT_TO_LIST (rat, i->flag))
            longjmp (i->env, 1);
        }
    }
  return 0;
}

static int
LOCtoPadPad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if ( !TEST_FLAG (i->flag, pad) &&
       i->layer == (TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE) &&
       PadPadIntersect (pad, i->pad, &pimri) &&
       ADD_PAD_TO_LIST (i->layer, pad, i->flag) ) {
    longjmp (i->env, 1);
  }
  return 0;
}

/*!
 * \brief Searches all LOs that are connected to the given pad on the
 * given layergroup.
 *
 * All found connections are added to the list.
 */
static bool
LookupLOConnectionsToPad (PadType *Pad, Cardinal LayerGroup, int flag, bool AndRats)
{
  Cardinal entry;
  struct lo_info info;
  BoxType search_box;

  if (!TEST_FLAG (SQUAREFLAG, Pad))
    return (LookupLOConnectionsToLine ((LineType *) Pad, LayerGroup, flag, false, AndRats));

  info.flag = flag;
  info.pad = Pad;
  search_box = expand_bounds ((BoxType *)info.pad);

  /* add the new rat lines */
  info.layer = LayerGroup;

  if (AndRats)
    {
      if (setjmp (info.env) == 0)
        r_search (PCB->Data->rat_tree, &search_box, NULL,
                  LOCtoPadRat_callback, &info);
      else
        return true;
    }

  /* loop over all layers of the group */
  for (entry = 0; entry < PCB->LayerGroups.Number[LayerGroup]; entry++)
    {
      Cardinal layer_no;
      LayerType *layer;

      layer_no = PCB->LayerGroups.Entries[LayerGroup][entry];
      layer = LAYER_PTR (layer_no);
      /* handle normal layers */
      if (layer_no < max_copper_layer)
        {
          info.layer = layer_no;
          /* add lines */
          if (setjmp (info.env) == 0)
            r_search (layer->line_tree, &search_box,
                      NULL, LOCtoPadLine_callback, &info);
          else
            return true;
          /* add arcs */
          if (setjmp (info.env) == 0)
            r_search (layer->arc_tree, &search_box,
                      NULL, LOCtoPadArc_callback, &info);
          else
            return true;
          /* add polygons */
          if (setjmp (info.env) == 0)
            r_search (layer->polygon_tree, &search_box,
                      NULL, LOCtoPadPoly_callback, &info);
          else
            return true;
        }
      else
        {
          /* handle special 'pad' layers */
          info.layer = layer_no - max_copper_layer;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pad_tree, &search_box, NULL,
                      LOCtoPadPad_callback, &info);
          else
            return true;
        }

    }
  return (false);
}

static int
LOCtoPolyLine_callback (const BoxType * b, void *cl)
{
  LineType *line = (LineType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, line) && IsLineInPolygon (line, i->polygon))
    {
      if (ADD_LINE_TO_LIST (i->layer, line, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPolyArc_callback (const BoxType * b, void *cl)
{
  ArcType *arc = (ArcType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!arc->Thickness)
    return 0;
  if (!TEST_FLAG (i->flag, arc) && IsArcInPolygon (arc, i->polygon))
    {
      if (ADD_ARC_TO_LIST (i->layer, arc, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPolyPad_callback (const BoxType * b, void *cl)
{
  PadType *pad = (PadType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, pad) && i->layer ==
      (TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE)
      && IsPadInPolygon (pad, i->polygon))
    {
      if (ADD_PAD_TO_LIST (i->layer, pad, i->flag))
        longjmp (i->env, 1);
    }
  return 0;
}

static int
LOCtoPolyRat_callback (const BoxType * b, void *cl)
{
  RatType *rat = (RatType *) b;
  struct lo_info *i = (struct lo_info *) cl;

  if (!TEST_FLAG (i->flag, rat))
    {
      if ((rat->Point1.X == (i->polygon->Clipped->contours->head.point[0]) &&
           rat->Point1.Y == (i->polygon->Clipped->contours->head.point[1]) &&
           rat->group1 == i->layer) ||
          (rat->Point2.X == (i->polygon->Clipped->contours->head.point[0]) &&
           rat->Point2.Y == (i->polygon->Clipped->contours->head.point[1]) &&
           rat->group2 == i->layer))
        if (ADD_RAT_TO_LIST (rat, i->flag))
          longjmp (i->env, 1);
    }
  return 0;
}


/*!
 * \brief Looks up LOs that are connected to the given polygon on the
 * given layergroup.
 *
 * All found connections are added to the list.
 */
static bool
LookupLOConnectionsToPolygon (PolygonType *Polygon, Cardinal LayerGroup, int flag, bool AndRats)
{
  Cardinal entry;
  struct lo_info info;
  BoxType search_box;

  if (!Polygon->Clipped)
    return false;

  info.flag = flag;
  info.polygon = Polygon;
  search_box = expand_bounds ((BoxType *)info.polygon);

  info.layer = LayerGroup;

  /* check rats */
  if (AndRats)
    {
      if (setjmp (info.env) == 0)
        r_search (PCB->Data->rat_tree, &search_box, NULL,
                  LOCtoPolyRat_callback, &info);
      else
        return true;
    }

/* loop over all layers of the group */
  for (entry = 0; entry < PCB->LayerGroups.Number[LayerGroup]; entry++)
    {
      Cardinal layer_no;
      LayerType *layer;

      layer_no = PCB->LayerGroups.Entries[LayerGroup][entry];
      layer = LAYER_PTR (layer_no);

      /* handle normal layers */
      if (layer_no < max_copper_layer)
        {
          GList *i;

          /* check all polygons */
          for (i = layer->Polygon; i != NULL; i = g_list_next (i))
            {
              PolygonType *polygon = i->data;
              if (!TEST_FLAG (flag, polygon)
                  && IsPolygonInPolygon (polygon, Polygon)
                  && ADD_POLYGON_TO_LIST (layer_no, polygon, flag))
                return true;
            }

          info.layer = layer_no;
          /* check all lines */
          if (setjmp (info.env) == 0)
            r_search (layer->line_tree, &search_box,
                      NULL, LOCtoPolyLine_callback, &info);
          else
            return true;
          /* check all arcs */
          if (setjmp (info.env) == 0)
            r_search (layer->arc_tree, &search_box,
                      NULL, LOCtoPolyArc_callback, &info);
          else
            return true;
        }
      else
        {
          info.layer = layer_no - max_copper_layer;
          if (setjmp (info.env) == 0)
            r_search (PCB->Data->pad_tree, &search_box,
                      NULL, LOCtoPolyPad_callback, &info);
          else
            return true;
        }
    }
  return (false);
}

/*!
 * \brief Checks if an arc has a connection to a polygon.
 *
 * - first check if the arc can intersect with the polygon by
 *   evaluating the bounding boxes.
 * - check the two end points of the arc. If none of them matches
 * - check all segments of the polygon against the arc.
 */
static bool
IsArcInPolygon (ArcType *Arc, PolygonType *Polygon)
{
  BoxType *Box = (BoxType *) Arc;

  /* arcs with clearance never touch polys */
  if (TEST_FLAG (CLEARPOLYFLAG, Polygon) && TEST_FLAG (CLEARLINEFLAG, Arc))
    return false;
  if (!Polygon->Clipped)
    return false;
  if (Box->X1 <= Polygon->Clipped->contours->xmax + Bloat
      && Box->X2 >= Polygon->Clipped->contours->xmin - Bloat
      && Box->Y1 <= Polygon->Clipped->contours->ymax + Bloat
      && Box->Y2 >= Polygon->Clipped->contours->ymin - Bloat)
    {
      POLYAREA *ap;

      if (!(ap = ArcPoly (Arc, Arc->Thickness + Bloat)))
        return false;           /* error */
      return isects (ap, Polygon, true);
    }
  return false;
}

/*!
 * \brief Checks if a line has a connection to a polygon.
 *
 * - first check if the line can intersect with the polygon by
 *   evaluating the bounding boxes
 * - check the two end points of the line. If none of them matches
 * - check all segments of the polygon against the line.
 */
static bool
IsLineInPolygon (LineType *Line, PolygonType *Polygon)
{
  BoxType *Box = (BoxType *) Line;
  POLYAREA *lp;

  /* lines with clearance never touch polygons */
  if (TEST_FLAG (CLEARPOLYFLAG, Polygon) && TEST_FLAG (CLEARLINEFLAG, Line))
    return false;
  if (!Polygon->Clipped)
    return false;
  if (TEST_FLAG(SQUAREFLAG,Line)&&(Line->Point1.X==Line->Point2.X||Line->Point1.Y==Line->Point2.Y))
     {
       Coord wid = (Line->Thickness + Bloat + 1) / 2;
       Coord x1, x2, y1, y2;

       x1 = MIN (Line->Point1.X, Line->Point2.X) - wid;
       y1 = MIN (Line->Point1.Y, Line->Point2.Y) - wid;
       x2 = MAX (Line->Point1.X, Line->Point2.X) + wid;
       y2 = MAX (Line->Point1.Y, Line->Point2.Y) + wid;
       return IsRectangleInPolygon (x1, y1, x2, y2, Polygon);
     }
  if (Box->X1 <= Polygon->Clipped->contours->xmax + Bloat
      && Box->X2 >= Polygon->Clipped->contours->xmin - Bloat
      && Box->Y1 <= Polygon->Clipped->contours->ymax + Bloat
      && Box->Y2 >= Polygon->Clipped->contours->ymin - Bloat)
    {
      if (!(lp = LinePoly (Line, Line->Thickness + Bloat)))
        return FALSE;           /* error */
      return isects (lp, Polygon, true);
    }
  return false;
}

/*!
 * \brief Checks if a pad connects to a non-clearing polygon.
 *
 * The polygon is assumed to already have been proven non-clearing.
 */
static bool
IsPadInPolygon (PadType *pad, PolygonType *polygon)
{
    return IsLineInPolygon ((LineType *) pad, polygon);
}

/*!
 * \brief Checks if a polygon has a connection to a second one.
 *
 * First check all points out of P1 against P2 and vice versa.
 * If both fail check all lines of P1 against the ones of P2.
 */
static bool
IsPolygonInPolygon (PolygonType *P1, PolygonType *P2)
{
  if (!P1->Clipped || !P2->Clipped)
    return false;
  assert (P1->Clipped->contours);
  assert (P2->Clipped->contours);

  /* first check if both bounding boxes intersect. If not, return quickly */
  if (P1->Clipped->contours->xmin - Bloat > P2->Clipped->contours->xmax ||
      P1->Clipped->contours->xmax + Bloat < P2->Clipped->contours->xmin ||
      P1->Clipped->contours->ymin - Bloat > P2->Clipped->contours->ymax ||
      P1->Clipped->contours->ymax + Bloat < P2->Clipped->contours->ymin)
    return false;

  /* first check un-bloated case */
  if (isects (P1->Clipped, P2, false))
    return TRUE;

  /* now the difficult case of bloated */
  if (Bloat > 0)
    {
      PLINE *c;
      for (c = P1->Clipped->contours; c; c = c->next)
        {
          LineType line;
          VNODE *v = &c->head;
          if (c->xmin - Bloat <= P2->Clipped->contours->xmax &&
              c->xmax + Bloat >= P2->Clipped->contours->xmin &&
              c->ymin - Bloat <= P2->Clipped->contours->ymax &&
              c->ymax + Bloat >= P2->Clipped->contours->ymin)
            {

              line.Point1.X = v->point[0];
              line.Point1.Y = v->point[1];
              line.Thickness = Bloat;
              /* Another Bloat is added by IsLineInPolygon, making the correct
               * 2x Bloat. Perhaps we should change it there, but doing so
               * breaks some other DRC checks which rely on the behaviour
               * in IsLineInPolygon.
               */
              line.Clearance = 0;
              line.Flags = NoFlags ();
              for (v = v->next; v != &c->head; v = v->next)
                {
                  line.Point2.X = v->point[0];
                  line.Point2.Y = v->point[1];
                  SetLineBoundingBox (&line);
                  if (IsLineInPolygon (&line, P2))
                    return (true);
                  line.Point1.X = line.Point2.X;
                  line.Point1.Y = line.Point2.Y;
                }
            }
        }
    }

  return (false);
}

/*!
 * \brief Writes the several names of an element to a file.
 */
static void
PrintElementNameList (ElementType *Element, FILE * FP)
{
  static DynamicStringType cname, pname, vname;

  CreateQuotedString (&cname, (char *)EMPTY (DESCRIPTION_NAME (Element)));
  CreateQuotedString (&pname, (char *)EMPTY (NAMEONPCB_NAME (Element)));
  CreateQuotedString (&vname, (char *)EMPTY (VALUE_NAME (Element)));
  fprintf (FP, "(%s %s %s)\n", cname.Data, pname.Data, vname.Data);
}

/*!
 * \brief Writes the several names of an element to a file.
 */
static void
PrintConnectionElementName (ElementType *Element, FILE * FP)
{
  fputs ("Element", FP);
  PrintElementNameList (Element, FP);
  fputs ("{\n", FP);
}

/*!
 * \brief Prints one {pin,pad,via}/element entry of connection lists.
 */
static void
PrintConnectionListEntry (char *ObjName, ElementType *Element,
                          bool FirstOne, FILE * FP)
{
  static DynamicStringType oname;

  CreateQuotedString (&oname, ObjName);
  if (FirstOne)
    fprintf (FP, "\t%s\n\t{\n", oname.Data);
  else
    {
      fprintf (FP, "\t\t%s ", oname.Data);
      if (Element)
        PrintElementNameList (Element, FP);
      else
        fputs ("(__VIA__)\n", FP);
    }
}

/*!
 * \brief Prints all found connections of a pads to file FP
 * the connections are stacked in 'PadList'.
 */
static void
PrintPadConnections (Cardinal Layer, FILE * FP, bool IsFirst)
{
  Cardinal i;
  PadType *ptr;

  if (!PadList[Layer].Number)
    return;

  /* the starting pad */
  if (IsFirst)
    {
      ptr = PADLIST_ENTRY (Layer, 0);
      if (ptr != NULL)
        PrintConnectionListEntry ((char *)UNKNOWN (ptr->Name), NULL, true, FP);
      else
        printf ("Skipping NULL ptr in 1st part of PrintPadConnections\n");
    }

  /* we maybe have to start with i=1 if we are handling the
   * starting-pad itself
   */
  for (i = IsFirst ? 1 : 0; i < PadList[Layer].Number; i++)
    {
      ptr = PADLIST_ENTRY (Layer, i);
      if (ptr != NULL)
        PrintConnectionListEntry ((char *)EMPTY (ptr->Name), (ElementType *)ptr->Element, false, FP);
      else
        printf ("Skipping NULL ptr in 2nd part of PrintPadConnections\n");
    }
}

/*!
 * \brief Prints all found connections of a pin to file FP
 * the connections are stacked in 'PVList'.
 */
static void
PrintPinConnections (FILE * FP, bool IsFirst)
{
  Cardinal i;
  PinType *pv;

  if (!PVList.Number)
    return;

  if (IsFirst)
    {
      /* the starting pin */
      pv = PVLIST_ENTRY (0);
      PrintConnectionListEntry ((char *)EMPTY (pv->Name), NULL, true, FP);
    }

  /* we maybe have to start with i=1 if we are handling the
   * starting-pin itself
   */
  for (i = IsFirst ? 1 : 0; i < PVList.Number; i++)
    {
      /* get the elements name or assume that its a via */
      pv = PVLIST_ENTRY (i);
      PrintConnectionListEntry ((char *)EMPTY (pv->Name), (ElementType *)pv->Element, false, FP);
    }
}

/*!
 * \brief Checks if all lists of new objects are handled.
 */
static bool
ListsEmpty (bool AndRats)
{
  bool empty;
  int i;

  empty = (PVList.Location >= PVList.Number);
  if (AndRats)
    empty = empty && (RatList.Location >= RatList.Number);
  for (i = 0; i < max_copper_layer && empty; i++)
    if (!LAYER_PTR (i)->no_drc)
      empty = empty && LineList[i].Location >= LineList[i].Number
        && ArcList[i].Location >= ArcList[i].Number
        && PolygonList[i].Location >= PolygonList[i].Number;
  return (empty);
}

static void
reassign_no_drc_flags (void)
{
  int layer;

  for (layer = 0; layer < max_copper_layer; layer++)
    {
      LayerType *l = LAYER_PTR (layer);
      l->no_drc = AttributeGet (l, "PCB::skip-drc") != NULL;
    }
}




/*!
 * \brief Loops till no more connections are found.
 */
static bool
DoIt (int flag, bool AndRats, bool AndDraw)
{
  bool newone = false;
  reassign_no_drc_flags ();
  do
    {
      /* lookup connections; these are the steps (2) to (4)
       * from the description
       */
      newone = LookupPVConnectionsToPVList (flag) ||
               LookupLOConnectionsToPVList (flag, AndRats) ||
               LookupLOConnectionsToLOList (flag, AndRats) ||
               LookupPVConnectionsToLOList (flag, AndRats);
      if (AndDraw)
        DrawNewConnections ();
    }
  while (!newone && !ListsEmpty (AndRats));
  if (AndDraw)
    Draw ();
  return (newone);
}

/*!
 * \brief Prints all unused pins of an element to file FP.
 */
static bool
PrintAndSelectUnusedPinsAndPadsOfElement (ElementType *Element, FILE * FP, int flag)
{
  bool first = true;
  Cardinal number;
  static DynamicStringType oname;

  /* check all pins in element */

  PIN_LOOP (Element);
  {
    if (!TEST_FLAG (HOLEFLAG, pin))
      {
        /* pin might have bee checked before, add to list if not */
        if (!TEST_FLAG (flag, pin) && FP)
          {
            int i;
            if (ADD_PV_TO_LIST (pin, flag))
              return true;
            DoIt (flag, true, true);
            number = PadList[TOP_SIDE].Number
              + PadList[BOTTOM_SIDE].Number + PVList.Number;
            /* the pin has no connection if it's the only
             * list entry; don't count vias
             */
            for (i = 0; i < PVList.Number; i++)
              if (!PVLIST_ENTRY (i)->Element)
                number--;
            if (number == 1)
              {
                /* output of element name if not already done */
                if (first)
                  {
                    PrintConnectionElementName (Element, FP);
                    first = false;
                  }

                /* write name to list and draw selected object */
                CreateQuotedString (&oname, (char *)EMPTY (pin->Name));
                fprintf (FP, "\t%s\n", oname.Data);
                SET_FLAG (SELECTEDFLAG, pin);
                DrawPin (pin);
              }

            /* reset found objects for the next pin */
            if (PrepareNextLoop (FP))
              return (true);
          }
      }
  }
  END_LOOP;

  /* check all pads in element */
  PAD_LOOP (Element);
  {
    /* lookup pad in list */
    /* pad might has bee checked before, add to list if not */
    if (!TEST_FLAG (flag, pad) && FP)
      {
        int i;
        if (ADD_PAD_TO_LIST (TEST_FLAG (ONSOLDERFLAG, pad)
                             ? BOTTOM_SIDE : TOP_SIDE, pad, flag))
          return true;
        DoIt (flag, true, true);
        number = PadList[TOP_SIDE].Number
          + PadList[BOTTOM_SIDE].Number + PVList.Number;
        /* the pin has no connection if it's the only
         * list entry; don't count vias
         */
        for (i = 0; i < PVList.Number; i++)
          if (!PVLIST_ENTRY (i)->Element)
            number--;
        if (number == 1)
          {
            /* output of element name if not already done */
            if (first)
              {
                PrintConnectionElementName (Element, FP);
                first = false;
              }

            /* write name to list and draw selected object */
            CreateQuotedString (&oname, (char *)EMPTY (pad->Name));
            fprintf (FP, "\t%s\n", oname.Data);
            SET_FLAG (SELECTEDFLAG, pad);
            DrawPad (pad);
          }

        /* reset found objects for the next pin */
        if (PrepareNextLoop (FP))
          return (true);
      }
  }
  END_LOOP;

  /* print separator if element has unused pins or pads */
  if (!first)
    {
      fputs ("}\n\n", FP);
      SEPARATE (FP);
    }
  return (false);
}

/*!
 * \brief Resets some flags for looking up the next pin/pad.
 */
static bool
PrepareNextLoop (FILE * FP)
{
  Cardinal layer;

  /* reset found LOs for the next pin */
  for (layer = 0; layer < max_copper_layer; layer++)
    {
      LineList[layer].Location = LineList[layer].Number = 0;
      ArcList[layer].Location = ArcList[layer].Number = 0;
      PolygonList[layer].Location = PolygonList[layer].Number = 0;
    }

  /* reset found pads */
  for (layer = 0; layer < 2; layer++)
    PadList[layer].Location = PadList[layer].Number = 0;

  /* reset PVs */
  PVList.Number = PVList.Location = 0;
  RatList.Number = RatList.Location = 0;

  return (false);
}

/*!
 * \brief Finds all connections to the pins of the passed element.
 *
 * The result is written to file FP.
 *
 * \return true if operation was aborted.
 */
static bool
PrintElementConnections (ElementType *Element, FILE * FP, int flag, bool AndDraw)
{
  PrintConnectionElementName (Element, FP);

  /* check all pins in element */
  PIN_LOOP (Element);
  {
    /* pin might have been checked before, add to list if not */
    if (TEST_FLAG (flag, pin))
      {
        PrintConnectionListEntry ((char *)EMPTY (pin->Name), NULL, true, FP);
        fputs ("\t\t__CHECKED_BEFORE__\n\t}\n", FP);
        continue;
      }
    if (ADD_PV_TO_LIST (pin, flag))
      return true;
    DoIt (flag, true, AndDraw);
    /* printout all found connections */
    PrintPinConnections (FP, true);
    PrintPadConnections (TOP_SIDE, FP, false);
    PrintPadConnections (BOTTOM_SIDE, FP, false);
    fputs ("\t}\n", FP);
    if (PrepareNextLoop (FP))
      return (true);
  }
  END_LOOP;

  /* check all pads in element */
  PAD_LOOP (Element);
  {
    Cardinal layer;
    /* pad might have been checked before, add to list if not */
    if (TEST_FLAG (flag, pad))
      {
        PrintConnectionListEntry ((char *)EMPTY (pad->Name), NULL, true, FP);
        fputs ("\t\t__CHECKED_BEFORE__\n\t}\n", FP);
        continue;
      }
    layer = TEST_FLAG (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE;
    if (ADD_PAD_TO_LIST (layer, pad, flag))
      return true;
    DoIt (flag, true, AndDraw);
    /* print all found connections */
    PrintPadConnections (layer, FP, true);
    PrintPadConnections (layer ==
                         (TOP_SIDE ? BOTTOM_SIDE : TOP_SIDE),
                         FP, false);
    PrintPinConnections (FP, false);
    fputs ("\t}\n", FP);
    if (PrepareNextLoop (FP))
      return (true);
  }
  END_LOOP;
  fputs ("}\n\n", FP);
  return (false);
}

/*!
 * \brief Draws all new connections which have been found since the
 * routine was called the last time.
 */
static void
DrawNewConnections (void)
{
  int i;
  Cardinal position;

  /* decrement 'i' to keep layerstack order */
  for (i = max_copper_layer - 1; i != -1; i--)
    {
      Cardinal layer = LayerStack[i];

      if (PCB->Data->Layer[layer].On)
        {
          /* draw all new lines */
          position = LineList[layer].DrawLocation;
          for (; position < LineList[layer].Number; position++)
            DrawLine (LAYER_PTR (layer), LINELIST_ENTRY (layer, position));
          LineList[layer].DrawLocation = LineList[layer].Number;

          /* draw all new arcs */
          position = ArcList[layer].DrawLocation;
          for (; position < ArcList[layer].Number; position++)
            DrawArc (LAYER_PTR (layer), ARCLIST_ENTRY (layer, position));
          ArcList[layer].DrawLocation = ArcList[layer].Number;

          /* draw all new polygons */
          position = PolygonList[layer].DrawLocation;
          for (; position < PolygonList[layer].Number; position++)
            DrawPolygon (LAYER_PTR (layer), POLYGONLIST_ENTRY (layer, position));
          PolygonList[layer].DrawLocation = PolygonList[layer].Number;
        }
    }

  /* draw all new pads */
  if (PCB->PinOn)
    for (i = 0; i < 2; i++)
      {
        position = PadList[i].DrawLocation;

        for (; position < PadList[i].Number; position++)
          DrawPad (PADLIST_ENTRY (i, position));
        PadList[i].DrawLocation = PadList[i].Number;
      }

  /* draw all new PVs; 'PVList' holds a list of pointers to the
   * sorted array pointers to PV data
   */
  while (PVList.DrawLocation < PVList.Number)
    {
      PinType *pv = PVLIST_ENTRY (PVList.DrawLocation);

      if (TEST_FLAG (PINFLAG, pv))
        {
          if (PCB->PinOn)
            DrawPin (pv);
        }
      else if (PCB->ViaOn)
        DrawVia (pv);
      PVList.DrawLocation++;
    }
  /* draw the new rat-lines */
  if (PCB->RatOn)
    {
      position = RatList.DrawLocation;
      for (; position < RatList.Number; position++)
        DrawRat (RATLIST_ENTRY (position));
      RatList.DrawLocation = RatList.Number;
    }
}

/*!
 * \brief Find all connections to pins within one element.
 */
void
LookupElementConnections (ElementType *Element, FILE * FP)
{
  /* reset all currently marked connections */
  User = true;
  ClearFlagOnAllObjects (true, FOUNDFLAG);
  InitConnectionLookup ();
  PrintElementConnections (Element, FP, FOUNDFLAG, true);
  SetChangedFlag (true);
  if (Settings.RingBellWhenFinished)
    gui->beep ();
  FreeConnectionLookupMemory ();
  IncrementUndoSerialNumber ();
  User = false;
  Draw ();
}

/*!
 * \brief Find all connections to pins of all element.
 */
void
LookupConnectionsToAllElements (FILE * FP)
{
  /* reset all currently marked connections */
  User = false;
  ClearFlagOnAllObjects (false, FOUNDFLAG);
  InitConnectionLookup ();

  ELEMENT_LOOP (PCB->Data);
  {
    /* break if abort dialog returned true */
    if (PrintElementConnections (element, FP, FOUNDFLAG, false))
      break;
    SEPARATE (FP);
    if (Settings.ResetAfterElement && n != 1)
      ClearFlagOnAllObjects (false, FOUNDFLAG);
  }
  END_LOOP;
  if (Settings.RingBellWhenFinished)
    gui->beep ();
  ClearFlagOnAllObjects (false, FOUNDFLAG);
  FreeConnectionLookupMemory ();
  Redraw ();
}

/*!
 * \brief Add the starting object to the list of found objects.
 */
static bool
ListStart (int type, void *ptr1, void *ptr2, void *ptr3, int flag)
{
  DumpList ();
  switch (type)
    {
    case PIN_TYPE:
    case VIA_TYPE:
      {
        if (ADD_PV_TO_LIST ((PinType *) ptr2, flag))
          return true;
        break;
      }

    case RATLINE_TYPE:
      {
        if (ADD_RAT_TO_LIST ((RatType *) ptr1, flag))
          return true;
        break;
      }

    case LINE_TYPE:
      {
        int layer = GetLayerNumber (PCB->Data,
                                    (LayerType *) ptr1);

        if (ADD_LINE_TO_LIST (layer, (LineType *) ptr2, flag))
          return true;
        break;
      }

    case ARC_TYPE:
      {
        int layer = GetLayerNumber (PCB->Data,
                                    (LayerType *) ptr1);

        if (ADD_ARC_TO_LIST (layer, (ArcType *) ptr2, flag))
          return true;
        break;
      }

    case POLYGON_TYPE:
      {
        int layer = GetLayerNumber (PCB->Data,
                                    (LayerType *) ptr1);

        if (ADD_POLYGON_TO_LIST (layer, (PolygonType *) ptr2, flag))
          return true;
        break;
      }

    case PAD_TYPE:
      {
        PadType *pad = (PadType *) ptr2;
        if (ADD_PAD_TO_LIST
            (TEST_FLAG
             (ONSOLDERFLAG, pad) ? BOTTOM_SIDE : TOP_SIDE, pad, flag))
          return true;
        break;
      }
    }
  return (false);
}


/*!
 * \brief Looks up all connections from the object at the given
 * coordinates the TheFlag (normally 'FOUNDFLAG') is set for all objects
 * found.
 *
 * The objects are re-drawn if AndDraw is true, also the action is
 * marked as undoable if AndDraw is true.
 */
void
LookupConnection (Coord X, Coord Y, bool AndDraw, Coord Range, int flag,
                  bool AndRats)
{
  void *ptr1, *ptr2, *ptr3;
  char *name;
  int type;

  /* check if there are any pins or pads at that position */

	reassign_no_drc_flags ();

  type
    = SearchObjectByLocation (LOOKUP_FIRST, &ptr1, &ptr2, &ptr3, X, Y, Range);
  if (type == NO_TYPE)
    {
      type = SearchObjectByLocation (
        LOOKUP_MORE & ~(AndRats ? 0 : RATLINE_TYPE),
        &ptr1, &ptr2, &ptr3, X, Y, Range);
      if (type == NO_TYPE)
        return;
      if (type & SILK_TYPE)
        {
          int laynum = GetLayerNumber (PCB->Data,
                                       (LayerType *) ptr1);

          /* don't mess with non-conducting objects! */
          if (laynum >= max_copper_layer || ((LayerType *)ptr1)->no_drc)
            return;
        }
    }

  name = ConnectionName (type, ptr1, ptr2);
  hid_actionl ("NetlistShow", name, NULL);

  User = AndDraw;
  InitConnectionLookup ();

  /* now add the object to the appropriate list and start scanning
   * This is step (1) from the description
   */
  ListStart (type, ptr1, ptr2, ptr3, flag);
  DoIt (flag, AndRats, AndDraw);
  if (User)
    IncrementUndoSerialNumber ();
  User = false;

  /* we are done */
  if (AndDraw)
    Draw ();
  if (AndDraw && Settings.RingBellWhenFinished)
    gui->beep ();
  FreeConnectionLookupMemory ();
}

/*!
 * \brief Find connections for rats nesting.
 *
 * Assumes InitConnectionLookup() has already been done.
 */
void
RatFindHook (int type, void *ptr1, void *ptr2, void *ptr3,
             bool undo, int flag, bool AndRats)
{
  User = undo;
  DumpList ();
  ListStart (type, ptr1, ptr2, ptr3, flag);
  DoIt (flag, AndRats, false);
  User = false;
}

/*!
 * \brief Find all unused pins of all elements.
 */
void
LookupUnusedPins (FILE * FP)
{
  /* reset all currently marked connections */
  User = true;
  ClearFlagOnAllObjects (true, FOUNDFLAG);
  InitConnectionLookup ();

  ELEMENT_LOOP (PCB->Data);
  {
    /* break if abort dialog returned true;
     * passing NULL as filedescriptor discards the normal output
     */
    if (PrintAndSelectUnusedPinsAndPadsOfElement (element, FP, FOUNDFLAG))
      break;
  }
  END_LOOP;

  if (Settings.RingBellWhenFinished)
    gui->beep ();
  FreeConnectionLookupMemory ();
  IncrementUndoSerialNumber ();
  User = false;
  Draw ();
}

/*!
 * \brief Resets all used flags of pins and vias.
 */
bool
ClearFlagOnPinsViasAndPads (bool AndDraw, int flag)
{
  bool change = false;

  VIA_LOOP (PCB->Data);
  {
    if (TEST_FLAG (flag, via))
      {
        if (AndDraw)
          AddObjectToFlagUndoList (VIA_TYPE, via, via, via);
        CLEAR_FLAG (flag, via);
        if (AndDraw)
          DrawVia (via);
        change = true;
      }
  }
  END_LOOP;
  ELEMENT_LOOP (PCB->Data);
  {
    PIN_LOOP (element);
    {
      if (TEST_FLAG (flag, pin))
        {
          if (AndDraw)
            AddObjectToFlagUndoList (PIN_TYPE, element, pin, pin);
          CLEAR_FLAG (flag, pin);
          if (AndDraw)
            DrawPin (pin);
          change = true;
        }
    }
    END_LOOP;
    PAD_LOOP (element);
    {
      if (TEST_FLAG (flag, pad))
        {
          if (AndDraw)
            AddObjectToFlagUndoList (PAD_TYPE, element, pad, pad);
          CLEAR_FLAG (flag, pad);
          if (AndDraw)
            DrawPad (pad);
          change = true;
        }
    }
    END_LOOP;
  }
  END_LOOP;
  if (change)
    SetChangedFlag (true);
  return change;
}

/*!
 * \brief Resets all used flags of LOs.
 */
bool
ClearFlagOnLinesAndPolygons (bool AndDraw, int flag)
{
  bool change = false;

  RAT_LOOP (PCB->Data);
  {
    if (TEST_FLAG (flag, line))
      {
        if (AndDraw)
          AddObjectToFlagUndoList (RATLINE_TYPE, line, line, line);
        CLEAR_FLAG (flag, line);
        if (AndDraw)
          DrawRat (line);
        change = true;
      }
  }
  END_LOOP;
  COPPERLINE_LOOP (PCB->Data);
  {
    if (TEST_FLAG (flag, line))
      {
        if (AndDraw)
          AddObjectToFlagUndoList (LINE_TYPE, layer, line, line);
        CLEAR_FLAG (flag, line);
        if (AndDraw)
          DrawLine (layer, line);
        change = true;
      }
  }
  ENDALL_LOOP;
  COPPERARC_LOOP (PCB->Data);
  {
    if (TEST_FLAG (flag, arc))
      {
        if (AndDraw)
          AddObjectToFlagUndoList (ARC_TYPE, layer, arc, arc);
        CLEAR_FLAG (flag, arc);
        if (AndDraw)
          DrawArc (layer, arc);
        change = true;
      }
  }
  ENDALL_LOOP;
  COPPERPOLYGON_LOOP (PCB->Data);
  {
    if (TEST_FLAG (flag, polygon))
      {
        if (AndDraw)
          AddObjectToFlagUndoList (POLYGON_TYPE, layer, polygon, polygon);
        CLEAR_FLAG (flag, polygon);
        if (AndDraw)
          DrawPolygon (layer, polygon);
        change = true;
      }
  }
  ENDALL_LOOP;
  if (change)
    SetChangedFlag (true);
  return change;
}

/*!
 * \brief Resets all found connections.
 */
bool
ClearFlagOnAllObjects (bool AndDraw, int flag)
{
  bool change = false;

  change = ClearFlagOnPinsViasAndPads  (AndDraw, flag) || change;
  change = ClearFlagOnLinesAndPolygons (AndDraw, flag) || change;

  return change;
}

/*!
 * \brief Dumps the list contents.
 */
static void
DumpList (void)
{
  Cardinal i;

  for (i = 0; i < 2; i++)
    {
      PadList[i].Number = 0;
      PadList[i].Location = 0;
      PadList[i].DrawLocation = 0;
    }

  PVList.Number = 0;
  PVList.Location = 0;

  for (i = 0; i < max_copper_layer; i++)
    {
      LineList[i].Location = 0;
      LineList[i].DrawLocation = 0;
      LineList[i].Number = 0;
      ArcList[i].Location = 0;
      ArcList[i].DrawLocation = 0;
      ArcList[i].Number = 0;
      PolygonList[i].Location = 0;
      PolygonList[i].DrawLocation = 0;
      PolygonList[i].Number = 0;
    }
  RatList.Number = 0;
  RatList.Location = 0;
  RatList.DrawLocation = 0;
}

struct drc_info
{
  int flag;
};

/*!
 * \brief Check for DRC violations on a single net starting from the pad
 * or pin.
 *
 * Sees if the connectivity changes when everything is bloated, or
 * shrunk.
 */
static bool
DRCFind (int What, void *ptr1, void *ptr2, void *ptr3)
{
  Coord x, y;
  int object_count;
  long int *object_id_list;
  int *object_type_list;
  DrcViolationType *violation;
  int flag;

  if (PCB->Shrink != 0)
    {
      Bloat = -PCB->Shrink;
      ListStart (What, ptr1, ptr2, ptr3, DRCFLAG | SELECTEDFLAG);
      DoIt (DRCFLAG | SELECTEDFLAG, true, false);
      /* ok now the shrunk net has the SELECTEDFLAG set */
      DumpList ();
      ListStart (What, ptr1, ptr2, ptr3, FOUNDFLAG);
      Bloat = 0;
      drc = true;               /* abort the search if we find anything not already found */
      if (DoIt (FOUNDFLAG, true, false))
        {
          /* We want to be sure to capture the location of the intersection
           * that's actually causing the violation, so we do this before the
           * below stuff that implements flag change undoability.  I think
           * it doesn't matter because it's just re-running the same pair
           * of tests at the same respective bloats, but there's no reason
           * to not capture the location (from the global) immediately after
           * the intersection that puts us on this violation branch.  */
          if ( pimri.X != PIMRI_UNSET ) {
            x = pimri.X;
            y = pimri.Y;
            /* Clear to avoid confuse ourselves next time */
            pimri.X = PIMRI_UNSET; 
          }
          else {
            /* If we end up here it means some code somewhere hasn't
             * been rewritten to set pimri yet.  Hopefully all the
             * intersection-detecting paths are fixed to set pimri now.
             * If debugging is enabled we fire an assertion if not, but in
             * normal use just fall back to using the object position.  */
            assert (false);
            LocateErrorObject (&x, &y);
          }
          DumpList ();
          /* make the flag changes undoable */
          ClearFlagOnAllObjects (false, FOUNDFLAG | SELECTEDFLAG);
          User = true;
          drc = false;
          Bloat = -PCB->Shrink;
          ListStart (What, ptr1, ptr2, ptr3, SELECTEDFLAG);
          DoIt (SELECTEDFLAG, true, true);
          DumpList ();
          ListStart (What, ptr1, ptr2, ptr3, FOUNDFLAG);
          Bloat = 0;
          drc = true;
          DoIt (FOUNDFLAG, true, true);
          DumpList ();
          User = false;
          drc = false;
          drcerr_count++;
          BuildObjectList (&object_count, &object_id_list, &object_type_list);
          violation = pcb_drc_violation_new (
              _("Potential for broken trace"),
              _("Insufficient overlap between objects can lead to broken tracks\n"
                "due to registration errors with old wheel style photo-plotters."),
              x, y,
              0,     /* ANGLE OF ERROR UNKNOWN */
              FALSE, /* MEASUREMENT OF ERROR UNKNOWN */
              0,     /* MAGNITUDE OF ERROR UNKNOWN */
              PCB->Shrink,
              object_count,
              object_id_list,
              object_type_list);
          append_drc_violation (violation);
          pcb_drc_violation_free (violation);
          free (object_id_list);
          free (object_type_list);

          if (!throw_drc_dialog())
            return (true);
          IncrementUndoSerialNumber ();
          Undo (true);
        }
      DumpList ();
    }
  /* now check the bloated condition */
  drc = false;
  ClearFlagOnAllObjects (false, FOUNDFLAG | SELECTEDFLAG);
  Bloat = 0;
  ListStart (What, ptr1, ptr2, ptr3, SELECTEDFLAG);
  DoIt (SELECTEDFLAG, true, false);
  DumpList ();
  flag = FOUNDFLAG;
  ListStart (What, ptr1, ptr2, ptr3, flag);
  Bloat = PCB->Bloat;
  drc = true;
  while (DoIt (flag, true, false))
    {
      /* We want to be sure to capture the location of the intersection
       * that's actually causing the violation, so we do this before the
       * below stuff that implements flag change undoability.  I think
       * it doesn't matter because it's just re-running the same pair
       * of tests at the same respective bloats, but there's no reason
       * to not capture the location (from the global) immediately after
       * the intersection that puts us on this violation branch.  */
      if ( pimri.X != PIMRI_UNSET ) {
        x = pimri.X;
        y = pimri.Y;
        /* Clear to avoid confuse ourselves next time */
        pimri.X = PIMRI_UNSET; 
      }
      else {
        /* If we end up here it means some code somewhere hasn't
         * been rewritten to set pimri yet.  Hopefully all the
         * intersection-detecting paths are fixed to set pimri now.
         * If debugging is enabled we fire an assertion if not, but in normal
         * use just fall back to using the object position.  */
        assert (false);
        LocateErrorObject (&x, &y);
      }
      DumpList ();
      /* make the flag changes undoable */
      ClearFlagOnAllObjects (false, FOUNDFLAG | SELECTEDFLAG);
      User = true;
      drc = false;
      Bloat = 0;
      ListStart (What, ptr1, ptr2, ptr3, SELECTEDFLAG);
      DoIt (SELECTEDFLAG, true, true);
      DumpList ();
      ListStart (What, ptr1, ptr2, ptr3, FOUNDFLAG);
      Bloat = PCB->Bloat;
      drc = true;
      DoIt (FOUNDFLAG, true, true);
      DumpList ();
      drcerr_count++;
      BuildObjectList (&object_count, &object_id_list, &object_type_list);
      violation = pcb_drc_violation_new (
          _("Copper areas too close"),
          _("Circuits that are too close may bridge during imaging, etching,\n"
            "plating, or soldering processes resulting in a direct short."),
          x, y,
          0,     /* ANGLE OF ERROR UNKNOWN */
          FALSE, /* MEASUREMENT OF ERROR UNKNOWN */
          0,     /* MAGNITUDE OF ERROR UNKNOWN */
          PCB->Bloat,
          object_count,
          object_id_list,
          object_type_list);
      append_drc_violation (violation);
      pcb_drc_violation_free (violation);
      free (object_id_list);
      free (object_type_list);
      User = false;
      drc = false;
      if (!throw_drc_dialog())
        return (true);
      IncrementUndoSerialNumber ();
      Undo (true);
      /* highlight the rest of the encroaching net so it's not reported again */
      flag = FOUNDFLAG | SELECTEDFLAG;
      Bloat = 0;
      ListStart (thing_type, thing_ptr1, thing_ptr2, thing_ptr3, flag);
      DoIt (flag, true, true);
      DumpList ();
      drc = true;
      Bloat = PCB->Bloat;
      ListStart (What, ptr1, ptr2, ptr3, flag);
    }
  drc = false;
  DumpList ();
  ClearFlagOnAllObjects (false, FOUNDFLAG | SELECTEDFLAG);
  return (false);
}

/*!
 * \brief DRC clearance callback.
 */
static int
drc_callback (DataType *data, LayerType *layer, PolygonType *polygon,
              int type, void *ptr1, void *ptr2, void *userdata)
{
  struct drc_info *i = (struct drc_info *) userdata;
  char *message;
  Coord x, y;
  int object_count;
  long int *object_id_list;
  int *object_type_list;
  DrcViolationType *violation;

  LineType *line = (LineType *) ptr2;
  ArcType *arc = (ArcType *) ptr2;
  PinType *pin = (PinType *) ptr2;
  PadType *pad = (PadType *) ptr2;

  SetThing (type, ptr1, ptr2, ptr2);

  switch (type)
    {
    case LINE_TYPE:
      if (line->Clearance < 2 * PCB->Bloat)
        {
          AddObjectToFlagUndoList (type, ptr1, ptr2, ptr2);
          SET_FLAG (i->flag, line);
          message = _("Line with insufficient clearance inside polygon\n");
          goto doIsBad;
        }
      break;
    case ARC_TYPE:
      if (arc->Clearance < 2 * PCB->Bloat)
        {
          AddObjectToFlagUndoList (type, ptr1, ptr2, ptr2);
          SET_FLAG (i->flag, arc);
          message = _("Arc with insufficient clearance inside polygon\n");
          goto doIsBad;
        }
      break;
    case PAD_TYPE:
      if (pad->Clearance && pad->Clearance < 2 * PCB->Bloat)
	if (IsPadInPolygon(pad,polygon))
	  {
	    AddObjectToFlagUndoList (type, ptr1, ptr2, ptr2);
	    SET_FLAG (i->flag, pad);
	    message = _("Pad with insufficient clearance inside polygon\n");
	    goto doIsBad;
	  }
      break;
    case PIN_TYPE:
      if (pin->Clearance && pin->Clearance < 2 * PCB->Bloat)
        {
          AddObjectToFlagUndoList (type, ptr1, ptr2, ptr2);
          SET_FLAG (i->flag, pin);
          message = _("Pin with insufficient clearance inside polygon\n");
          goto doIsBad;
        }
      break;
    case VIA_TYPE:
      if (pin->Clearance && pin->Clearance < 2 * PCB->Bloat)
        {
          AddObjectToFlagUndoList (type, ptr1, ptr2, ptr2);
          SET_FLAG (i->flag, pin);
          message = _("Via with insufficient clearance inside polygon\n");
          goto doIsBad;
        }
      break;
    default:
      Message ("hace: Bad Plow object in callback\n");
    }
  return 0;

doIsBad:
  AddObjectToFlagUndoList (POLYGON_TYPE, layer, polygon, polygon);
  SET_FLAG (FOUNDFLAG, polygon);
  DrawPolygon (layer, polygon);
  DrawObject (type, ptr1, ptr2);
  drcerr_count++;
  LocateErrorObject (&x, &y);
  BuildObjectList (&object_count, &object_id_list, &object_type_list);
  violation = pcb_drc_violation_new (message,
                                     _("Circuits that are too close may bridge during imaging, etching,\n"
                                       "plating, or soldering processes resulting in a direct short."),
                                     x, y,
                                     0,     /* ANGLE OF ERROR UNKNOWN */
                                     FALSE, /* MEASUREMENT OF ERROR UNKNOWN */
                                     0,     /* MAGNITUDE OF ERROR UNKNOWN */
                                     PCB->Bloat,
                                     object_count,
                                     object_id_list,
                                     object_type_list);
  append_drc_violation (violation);
  pcb_drc_violation_free (violation);
  free (object_id_list);
  free (object_type_list);

  if (!throw_drc_dialog())
    return 1;

  IncrementUndoSerialNumber ();
  Undo (true);
  return 0;
}

/*!
 * \brief Check for DRC violations.
 *
 * See if the connectivity changes when everything is bloated, or shrunk.
 */
int
DRCAll (void)
{
  Coord x, y;
  int object_count;
  long int *object_id_list;
  int *object_type_list;
  DrcViolationType *violation;
  int tmpcnt;
  int nopastecnt = 0;
  bool IsBad;
  struct drc_info info;

  /* Make sure pimri starts out unset.  It's should be cleared by code that
   * consumes it's setting, so this is just defensive programming.  */
  pimri.X = PIMRI_UNSET;   /* Mark the entire pimri value as unset */

  reset_drc_dialog_message();

  /* This phony violation informs user about what DRC does NOT catch.  */
  violation
    = pcb_drc_violation_new (
        _("WARNING: DRC doesn't catch everything"),
        _("Detection of outright shorts, missing connections, etc.\n"
          "is handled via rat's nest addition.  To catch these problems,\n"
          "display the message log using Window->Message Log, then use\n"
          "Connects->Optimize rats nest (O hotkey) and watch for messages.\n"),
        /* All remaining arguments are not relevant to this application.  */
        0, 0, 0, TRUE, 0, 0, 0, NULL, NULL);
  append_drc_violation (violation);
  pcb_drc_violation_free (violation);
  if (!throw_drc_dialog())
    return (true);

  IsBad = false;
  drcerr_count = 0;
  SaveStackAndVisibility ();
  ResetStackAndVisibility ();
  hid_action ("LayersChanged");
  InitConnectionLookup ();

  if (ClearFlagOnAllObjects (true, FOUNDFLAG | DRCFLAG | SELECTEDFLAG))
    {
      IncrementUndoSerialNumber ();
      Draw ();
    }

  User = false;

  ELEMENT_LOOP (PCB->Data);
  {
    PIN_LOOP (element);
    {
      if (!TEST_FLAG (DRCFLAG, pin)
          && DRCFind (PIN_TYPE, (void *) element, (void *) pin, (void *) pin))
        {
          IsBad = true;
          break;
        }
    }
    END_LOOP;
    if (IsBad)
      break;
    PAD_LOOP (element);
    {

      /* count up how many pads have no solderpaste openings */
      if (TEST_FLAG (NOPASTEFLAG, pad))
	nopastecnt++;

      if (!TEST_FLAG (DRCFLAG, pad)
          && DRCFind (PAD_TYPE, (void *) element, (void *) pad, (void *) pad))
        {
          IsBad = true;
          break;
        }
    }
    END_LOOP;
    if (IsBad)
      break;
  }
  END_LOOP;
  if (!IsBad)
    VIA_LOOP (PCB->Data);
  {
    if (!TEST_FLAG (DRCFLAG, via)
        && DRCFind (VIA_TYPE, (void *) via, (void *) via, (void *) via))
      {
        IsBad = true;
        break;
      }
  }
  END_LOOP;

  ClearFlagOnAllObjects (false, IsBad ? DRCFLAG : (FOUNDFLAG | DRCFLAG | SELECTEDFLAG));
  info.flag = SELECTEDFLAG;
  /* check minimum widths and polygon clearances */
  if (!IsBad)
    {
      COPPERLINE_LOOP (PCB->Data);
      {
        /* check line clearances in polygons */
        if (PlowsPolygon (PCB->Data, LINE_TYPE, layer, line, drc_callback, &info))
          {
            IsBad = true;
            break;
          }
        if (line->Thickness < PCB->minWid)
          {
            AddObjectToFlagUndoList (LINE_TYPE, layer, line, line);
            SET_FLAG (SELECTEDFLAG, line);
            DrawLine (layer, line);
            drcerr_count++;
            SetThing (LINE_TYPE, layer, line, line);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Line width is too thin"),
                                               _("Process specifications dictate a minimum feature-width\n"
                                                 "that can reliably be reproduced"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               line->Thickness,
                                               PCB->minWid,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
      }
      ENDALL_LOOP;
    }
  if (!IsBad)
    {
      COPPERARC_LOOP (PCB->Data);
      {
        if (PlowsPolygon (PCB->Data, ARC_TYPE, layer, arc, drc_callback, &info))
          {
            IsBad = true;
            break;
          }
        if (arc->Thickness < PCB->minWid)
          {
            AddObjectToFlagUndoList (ARC_TYPE, layer, arc, arc);
            SET_FLAG (SELECTEDFLAG, arc);
            DrawArc (layer, arc);
            drcerr_count++;
            SetThing (ARC_TYPE, layer, arc, arc);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Arc width is too thin"),
                                               _("Process specifications dictate a minimum feature-width\n"
                                                 "that can reliably be reproduced"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               arc->Thickness,
                                               PCB->minWid,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
      }
      ENDALL_LOOP;
    }
  if (!IsBad)
    {
      ALLPIN_LOOP (PCB->Data);
      {
        if (PlowsPolygon (PCB->Data, PIN_TYPE, element, pin, drc_callback, &info))
          {
            IsBad = true;
            break;
          }
        if (!TEST_FLAG (HOLEFLAG, pin) &&
            pin->Thickness - pin->DrillingHole < 2 * PCB->minRing)
          {
            AddObjectToFlagUndoList (PIN_TYPE, element, pin, pin);
            SET_FLAG (SELECTEDFLAG, pin);
            DrawPin (pin);
            drcerr_count++;
            SetThing (PIN_TYPE, element, pin, pin);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Pin annular ring too small"),
                                               _("Annular rings that are too small may erode during etching,\n"
                                                 "resulting in a broken connection"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               (pin->Thickness - pin->DrillingHole) / 2,
                                               PCB->minRing,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
        if (pin->DrillingHole < PCB->minDrill)
          {
            AddObjectToFlagUndoList (PIN_TYPE, element, pin, pin);
            SET_FLAG (SELECTEDFLAG, pin);
            DrawPin (pin);
            drcerr_count++;
            SetThing (PIN_TYPE, element, pin, pin);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Pin drill size is too small"),
                                               _("Process rules dictate the minimum drill size which can be used"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               pin->DrillingHole,
                                               PCB->minDrill,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
      }
      ENDALL_LOOP;
    }
  if (!IsBad)
    {
      ALLPAD_LOOP (PCB->Data);
      {
        if (PlowsPolygon (PCB->Data, PAD_TYPE, element, pad, drc_callback, &info))
          {
            IsBad = true;
            break;
          }
        if (pad->Thickness < PCB->minWid)
          {
            AddObjectToFlagUndoList (PAD_TYPE, element, pad, pad);
            SET_FLAG (SELECTEDFLAG, pad);
            DrawPad (pad);
            drcerr_count++;
            SetThing (PAD_TYPE, element, pad, pad);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Pad is too thin"),
                                               _("Pads which are too thin may erode during etching,\n"
                                                  "resulting in a broken or unreliable connection"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               pad->Thickness,
                                               PCB->minWid,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
      }
      ENDALL_LOOP;
    }
  if (!IsBad)
    {
      VIA_LOOP (PCB->Data);
      {
        if (PlowsPolygon (PCB->Data, VIA_TYPE, via, via, drc_callback, &info))
          {
            IsBad = true;
            break;
          }
        if (!TEST_FLAG (HOLEFLAG, via) &&
            via->Thickness - via->DrillingHole < 2 * PCB->minRing)
          {
            AddObjectToFlagUndoList (VIA_TYPE, via, via, via);
            SET_FLAG (SELECTEDFLAG, via);
            DrawVia (via);
            drcerr_count++;
            SetThing (VIA_TYPE, via, via, via);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Via annular ring too small"),
                                               _("Annular rings that are too small may erode during etching,\n"
                                                 "resulting in a broken connection"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               (via->Thickness - via->DrillingHole) / 2,
                                               PCB->minRing,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
        if (via->DrillingHole < PCB->minDrill)
          {
            AddObjectToFlagUndoList (VIA_TYPE, via, via, via);
            SET_FLAG (SELECTEDFLAG, via);
            DrawVia (via);
            drcerr_count++;
            SetThing (VIA_TYPE, via, via, via);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Via drill size is too small"),
                                               _("Process rules dictate the minimum drill size which can be used"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               via->DrillingHole,
                                               PCB->minDrill,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
            IncrementUndoSerialNumber ();
            Undo (false);
          }
      }
      END_LOOP;
    }

  FreeConnectionLookupMemory ();
  Bloat = 0;

  /* check silkscreen minimum widths outside of elements */
  /* XXX - need to check text and polygons too! */
  if (!IsBad)
    {
      SILKLINE_LOOP (PCB->Data);
      {
        if (line->Thickness < PCB->minSlk)
          {
            SET_FLAG (SELECTEDFLAG, line);
            DrawLine (layer, line);
            drcerr_count++;
            SetThing (LINE_TYPE, layer, line, line);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);
            violation = pcb_drc_violation_new (_("Silk line is too thin"),
                                               _("Process specifications dictate a minimum silkscreen\n"
                                               "feature-width that can reliably be reproduced"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               line->Thickness,
                                               PCB->minSlk,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
          }
      }
      ENDALL_LOOP;
    }

  /* check silkscreen minimum widths inside of elements */
  /* XXX - need to check text and polygons too! */
  if (!IsBad)
    {
      ELEMENT_LOOP (PCB->Data);
      {
        tmpcnt = 0;
        ELEMENTLINE_LOOP (element);
        {
          if (line->Thickness < PCB->minSlk)
            tmpcnt++;
        }
        END_LOOP;
        if (tmpcnt > 0)
          {
            char *title;
            char *name;
            char *buffer;
            int buflen;

            SET_FLAG (SELECTEDFLAG, element);
            DrawElement (element);
            drcerr_count++;
            SetThing (ELEMENT_TYPE, element, element, element);
            LocateErrorObject (&x, &y);
            BuildObjectList (&object_count, &object_id_list, &object_type_list);

            title = _("Element %s has %i silk lines which are too thin");
            name = (char *)UNKNOWN (NAMEONPCB_NAME (element));

            /* -4 is for the %s and %i place-holders */
            /* +11 is the max printed length for a 32 bit integer */
            /* +1 is for the \0 termination */
            buflen = strlen (title) - 4 + strlen (name) + 11 + 1;
            buffer = (char *)malloc (buflen);
            snprintf (buffer, buflen, title, name, tmpcnt);

            violation = pcb_drc_violation_new (buffer,
                                               _("Process specifications dictate a minimum silkscreen\n"
                                               "feature-width that can reliably be reproduced"),
                                               x, y,
                                               0,    /* ANGLE OF ERROR UNKNOWN */
                                               TRUE, /* MEASUREMENT OF ERROR KNOWN */
                                               0,    /* MINIMUM OFFENDING WIDTH UNKNOWN */
                                               PCB->minSlk,
                                               object_count,
                                               object_id_list,
                                               object_type_list);
            free (buffer);
            append_drc_violation (violation);
            pcb_drc_violation_free (violation);
            free (object_id_list);
            free (object_type_list);
            if (!throw_drc_dialog())
              {
                IsBad = true;
                break;
              }
          }
      }
      END_LOOP;
    }


  if (IsBad)
    {
      IncrementUndoSerialNumber ();
    }


  RestoreStackAndVisibility ();
  hid_action ("LayersChanged");
  gui->invalidate_all ();

  if (nopastecnt > 0) 
    {
      Message (ngettext ("Warning: %d pad has the nopaste flag set.\n",
                         "Warning: %d pads have the nopaste flag set.\n",
			 nopastecnt), nopastecnt);
    }
  return IsBad ? -drcerr_count : drcerr_count;
}

/*!
 * \brief Locate the coordinatates of offending item (thing).
 */
static void
LocateErrorObject (Coord *x, Coord *y)
{
  switch (thing_type)
    {
    case LINE_TYPE:
      {
        LineType *line = (LineType *) thing_ptr3;
        *x = (line->Point1.X + line->Point2.X) / 2;
        *y = (line->Point1.Y + line->Point2.Y) / 2;
        break;
      }
    case ARC_TYPE:
      {
        ArcType *arc = (ArcType *) thing_ptr3;
        *x = arc->X;
        *y = arc->Y;
        break;
      }
    case POLYGON_TYPE:
      {
        PolygonType *polygon = (PolygonType *) thing_ptr3;
        *x =
          (polygon->Clipped->contours->xmin +
           polygon->Clipped->contours->xmax) / 2;
        *y =
          (polygon->Clipped->contours->ymin +
           polygon->Clipped->contours->ymax) / 2;
        break;
      }
    case PIN_TYPE:
    case VIA_TYPE:
      {
        PinType *pin = (PinType *) thing_ptr3;
        *x = pin->X;
        *y = pin->Y;
        break;
      }
    case PAD_TYPE:
      {
        PadType *pad = (PadType *) thing_ptr3;
        *x = (pad->Point1.X + pad->Point2.X) / 2;
        *y = (pad->Point1.Y + pad->Point2.Y) / 2;
        break;
      }
    case ELEMENT_TYPE:
      {
        ElementType *element = (ElementType *) thing_ptr3;
        *x = element->MarkX;
        *y = element->MarkY;
        break;
      }
    default:
      return;
    }
}


/*!
 * \brief Build a list of the of offending items by ID.
 *
 * (Currently just "thing").
 */
static void
BuildObjectList (int *object_count, long int **object_id_list, int **object_type_list)
{
  *object_count = 0;
  *object_id_list = NULL;
  *object_type_list = NULL;

  switch (thing_type)
    {
    case LINE_TYPE:
    case ARC_TYPE:
    case POLYGON_TYPE:
    case PIN_TYPE:
    case VIA_TYPE:
    case PAD_TYPE:
    case ELEMENT_TYPE:
    case RATLINE_TYPE:
      *object_count = 1;
      *object_id_list = (long int *)malloc (sizeof (long int));
      *object_type_list = (int *)malloc (sizeof (int));
      **object_id_list = ((AnyObjectType *)thing_ptr3)->ID;
      **object_type_list = thing_type;
      return;

    default:
      fprintf (stderr,
	       _("Internal error in BuildObjectList: unknown object type %i\n"),
	       thing_type);
    }
}


/*!
 * \brief Center the display to show the offending item (thing).
 */
static void
GotoError (void)
{
  Coord X, Y;

  LocateErrorObject (&X, &Y);

  switch (thing_type)
    {
    case LINE_TYPE:
    case ARC_TYPE:
    case POLYGON_TYPE:
      ChangeGroupVisibility (
          GetLayerNumber (PCB->Data, (LayerType *) thing_ptr1),
          true, true);
    }
  CenterDisplay (X, Y, false);
}

void
InitConnectionLookup (void)
{
  InitComponentLookup ();
  InitLayoutLookup ();
}

void
FreeConnectionLookupMemory (void)
{
  FreeComponentLookupMemory ();
  FreeLayoutLookupMemory ();
}

#ifdef PCB_UNIT_TEST

#include "file.h"

/* Fixture for tests involving exactly two lines.  */
typedef struct {
   LineType *line1;
   LineType *line2;
} LineLineFixture;

static void
line_line_intersection_tests_fixture_setup (
    LineLineFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for line-line intersection tests by loading two lines
   * from the given file.  */
 
  int return_code = LoadPCB ((char *) pcb_file);
  g_assert (return_code == 0);

  /* Load the lines */
  fixture->line1 = NULL;
  fixture->line2 = NULL;
  COPPERLINE_LOOP (PCB->Data);
  {
    if ( fixture->line1 == NULL ) {
      fixture->line1 = line;
    }
    else if ( fixture->line2 == NULL ) {
      fixture->line2 = line;
    }
    else {
      /* Because test case should not have >2 lines */
      g_assert_not_reached ();
    }
  }
  ENDALL_LOOP;
  /* Because test case should not have <2 lines */
  g_assert (fixture->line1 != NULL && fixture->line2 != NULL);
}

static void
expect_line_line_intersection (
    LineLineFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_true (
      LineLineIntersect (fixture->line1, fixture->line2, NULL) );
}

static void
expect_no_line_line_intersection (
    LineLineFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_false (
      LineLineIntersect (fixture->line1, fixture->line2, NULL) );
}

/* Add Line-Line Intersection Test Test */
#define ALLITT(case_ending, expectation)                      \
  g_test_add (                                                \
      "/line_line_intersection/" case_ending,                 \
      LineLineFixture,                                        \
      "test_data/line_line_intersection_" case_ending ".pcb", \
      line_line_intersection_tests_fixture_setup,             \
      expectation,                                            \
      NULL )

/* Fixture for tests involving exactly one line and one arc.  */
typedef struct {
   ArcType  *the_arc;    /* associated tests involve exactly one arc */
   LineType *the_line;   /* associated tests involve exactly one line */
} LineArcFixture;

static void
line_arc_intersection_tests_fixture_setup (
    LineArcFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for line-arc intersection tests by loading one of each
   * from the given file.  */
 
  int return_code = LoadPCB ((char *) pcb_file);
  g_assert (return_code == 0);

  /* Load the line */
  fixture->the_line = NULL;
  COPPERLINE_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 line */
    g_assert (fixture->the_line == NULL);
    fixture->the_line = line; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 line */
  g_assert (fixture->the_line != NULL);

  /* Load the arc */
  fixture->the_arc = NULL;
  COPPERARC_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 arc */
    g_assert (fixture->the_arc == NULL);
    fixture->the_arc = arc; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 arc */
  g_assert (fixture->the_arc != NULL);
}

static void
expect_line_arc_intersection (
    LineArcFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_true (
      LineArcIntersect (fixture->the_line, fixture->the_arc, NULL) );
}

static void
expect_no_line_arc_intersection (
    LineArcFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_false (
      LineArcIntersect (fixture->the_line, fixture->the_arc, NULL) );
}

/* Add Line-Arc Intersection Test Test */
#define ALAITT(case_ending, expectation)                     \
  g_test_add (                                               \
      "/line_arc_intersection/" case_ending,                 \
      LineArcFixture,                                        \
      "test_data/line_arc_intersection_" case_ending ".pcb", \
      line_arc_intersection_tests_fixture_setup,             \
      expectation,                                           \
      NULL )

/* Fixture for tests involving exactly one pin and one line.  */
typedef struct {
   PinType  *the_pin;    /* associated tests involve exactly one pin */
   LineType *the_line;   /* associated tests involve exactly one line */
} PinLineFixture;

static void
pin_line_intersection_tests_fixture_setup (
    PinLineFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for pine-line intersection tests by loading one of each
   * from the given file.  */
 
  int return_code = LoadPCB ((char *) pcb_file);
  g_assert (return_code == 0);

  /* Load the pin */
  fixture->the_pin = NULL;
  ALLPIN_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 pin */
    g_assert (fixture->the_pin == NULL);
    fixture->the_pin = pin; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 pin */
  g_assert (fixture->the_pin != NULL);

  /* Load the line */
  fixture->the_line = NULL;
  COPPERLINE_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 line */
    g_assert (fixture->the_line == NULL);
    fixture->the_line = line; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 line */
  g_assert (fixture->the_line != NULL);
}

static void
expect_pin_line_intersection (
    PinLineFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_true (
      PinLineIntersect (fixture->the_pin, fixture->the_line, NULL) );
}

static void
expect_no_pin_line_intersection (
    PinLineFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_false (
      PinLineIntersect (fixture->the_pin, fixture->the_line, NULL) );
}

/* Add Pin-Line Intersection Test Test */
#define APLITT(case_ending, expectation)                     \
  g_test_add (                                               \
      "/pin_line_intersection/" case_ending,                 \
      PinLineFixture,                                        \
      "test_data/pin_line_intersection_" case_ending ".pcb", \
      pin_line_intersection_tests_fixture_setup,             \
      expectation,                                           \
      NULL )

/* Fixture for tests involving exactly one line and one pad.  */
typedef struct {
   LineType  *the_line;   /* associated tests involve exactly one line */
   PadType *the_pad;      /* associated tests involve exactly one pad */
} LinePadFixture;

static void
line_pad_intersection_tests_fixture_setup (
    LinePadFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for line-pad intersection tests by loading one of each
   * from the given file.  */
 
  int return_code = LoadPCB ((char *) pcb_file);
  g_assert (return_code == 0);

  /* Load the line */
  fixture->the_line = NULL;
  COPPERLINE_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 line */
    g_assert (fixture->the_line == NULL);
    fixture->the_line = line; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 line */
  g_assert (fixture->the_line != NULL);
  
  /* Load the pad */
  fixture->the_pad = NULL;
  ALLPAD_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 pad */
    g_assert (fixture->the_pad == NULL);
    fixture->the_pad = pad; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 pad */
  g_assert (fixture->the_pad != NULL);
}

static void
expect_line_pad_intersection (
    LinePadFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_true (
      LinePadIntersect (fixture->the_line, fixture->the_pad, NULL) );
}

static void
expect_no_line_pad_intersection (
    LinePadFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_false (
      LinePadIntersect (fixture->the_line, fixture->the_pad, NULL) );
}

/* Add Line-Pad Intersection Test Test */
#define ALPITT(case_ending, expectation)                     \
  g_test_add (                                               \
      "/line_pad_intersection/" case_ending,                 \
      LinePadFixture,                                        \
      "test_data/line_pad_intersection_" case_ending ".pcb", \
      line_pad_intersection_tests_fixture_setup,             \
      expectation,                                           \
      NULL )

/* Fixture for tests involving exactly one arc and one pad.  */
typedef struct {
   ArcType *the_arc;   /* associated tests involve exactly one arc */
   PadType *the_pad;    /* associated tests involve exactly one pad */
} ArcPadFixture;

static void
arc_pad_intersection_tests_fixture_setup (
    ArcPadFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for arc-pad intersection tests by loading one of each
   * from the given file.  */
 
  int return_code = LoadPCB ((char *) pcb_file);
  g_assert (return_code == 0);
  
  /* Load the arc */
  fixture->the_arc = NULL;
  COPPERARC_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 arc */
    g_assert (fixture->the_arc == NULL);
    fixture->the_arc = arc; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 arc */
  g_assert (fixture->the_arc != NULL);

  /* Load the pad */
  fixture->the_pad = NULL;
  ALLPAD_LOOP (PCB->Data);
  {
    /* Because test case should not have >1 pad */
    g_assert (fixture->the_pad == NULL);
    fixture->the_pad = pad; 
  }
  ENDALL_LOOP;
  /* Because test case should not have <1 pad */
  g_assert (fixture->the_pad != NULL);
}

static void
expect_arc_pad_intersection (
    ArcPadFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_true (
      ArcPadIntersect (fixture->the_arc, fixture->the_pad, NULL) );
}

static void
expect_no_arc_pad_intersection (
    ArcPadFixture *fixture,
    gconstpointer pcb_file )
{
  g_assert_false (
      ArcPadIntersect (fixture->the_arc, fixture->the_pad, NULL) );
}

/* Add Arc-Pad Intersection Test Test */
#define AAPITT(case_ending, expectation)                    \
  g_test_add (                                              \
      "/arc_pad_intersection/" case_ending,                 \
      ArcPadFixture,                                        \
      "test_data/arc_pad_intersection_" case_ending ".pcb", \
      arc_pad_intersection_tests_fixture_setup,             \
      expectation,                                          \
      NULL )

static void
bloated_line_line_intersection_tests_fixture_setup (
    LineLineFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for bloated line-line intersection tests by loading two
   * lines from the given file and setting the Bloat global.  Note that the
   * LineLineFixture is reused here since no new (non-global)
   * data is required.  */

  line_line_intersection_tests_fixture_setup (fixture, pcb_file);

  /* This bloat setting is manually set to produce interesting intersection
   * changes in the data files used for this test.  If two thingies in the
   * data file are less than twice this far apart (i.e. less than about
   * 0.0042 mm apart, an intersection is created.  This assumes that the
   * internal units are nanometers.  */
  Bloat = 4200;
}

/* Add Bloated Line-Line Intersection Test Test.  Note that these use the same
 * test functions as the line-line intersection tests, but the global context
 * provided by the fixture is different.  */
#define ABLLITT(case_ending, expectation)                            \
  g_test_add (                                                       \
      "/bloated_line_line_intersection/" case_ending,                \
      LineLineFixture,                                               \
      "test_data/bloated_line_line_intersection_" case_ending".pcb", \
      bloated_line_line_intersection_tests_fixture_setup,            \
      expectation,                                                   \
      NULL )

static void
bloated_line_arc_intersection_tests_fixture_setup (
    LineArcFixture *fixture,
    gconstpointer pcb_file )
{
  /* Set up a fixture for bloated line-arc intersection tests by loading two
   * lines from the given file and setting the Bloat global.  Note that the
   * LineArcFixture is reused here since no new (non-global)
   * data is required.  */

  line_arc_intersection_tests_fixture_setup (fixture, pcb_file);

  /* This bloat setting is manually set to produce interesting intersection
   * changes in the data files used for this test.  If two thingies in the
   * data file are less than twice this far apart (i.e. less than about
   * 0.0042 mm apart, an intersection is created.  This assumes that the
   * internal units are nanometers.  */
  Bloat = 4200;
}

static void
bloated_line_arc_intersection_tests_fixture_tear_down (
    LineArcFixture *fixture,
    gconstpointer pcb_file )
{
  /* Tear down the bloated lin-arc intersection test fixture.  */ 

  /* Restore the default Bloat setting to prevent inter-test effects.  */
  Bloat = 0.0;
}

/* Add Bloated Line-Arc Intersection Test Test.  Note that these use the same
 * test functions as the line-line intersection tests, but the global context
 * provided by the fixture is different.  */
#define ABLAITT(case_ending, expectation)                           \
  g_test_add (                                                      \
      "/bloated_line_arc_intersection/" case_ending,                \
      LineArcFixture,                                               \
      "test_data/bloated_line_arc_intersection_" case_ending".pcb", \
      bloated_line_arc_intersection_tests_fixture_setup,            \
      expectation,                                                  \
      bloated_line_arc_intersection_tests_fixture_tear_down )

/* The fixtures for the intersection position tests need the file, and the
 * tests themselves need the the expected_pii.  The g_test interface sends the
 * same single pointer argument to both, so we marshall them together.  */
typedef struct {
  char *pcb_file;
  PointType expected_pii;
} MarshalledPositionTestArgs;

static void
line_line_intersection_position_tests_fixture_setup (
    LineLineFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Set up a fixture for line-line intersection position tests.  */

  char *pcb_file = ((MarshalledPositionTestArgs *) marshalled_args)->pcb_file;

  line_line_intersection_tests_fixture_setup (fixture, pcb_file);
}

/* WARNING: despite the signature, we deallocate stuff pointed to by
 * marshaleld_args.  */
static void
line_line_intersection_position_tests_fixture_tear_down (
    LineLineFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Tear down a line-line intersection position test fixutre.  Weirdly, the
   * memory for the test fixture and arguments are freed here, even though they
   * were allocated before the corresponding setup fixutre.  This is a
   * consequence of the fact that glib tests are first "add"ed, but run later.
   * Each test need in this family needs it's own unique argument at add-time,
   * and we only know that we're done with that argument when the tear-down
   * function finally fires.  */

  MarshalledPositionTestArgs *ma = (gpointer) marshalled_args;

  g_free (ma->pcb_file);
  g_free (ma);
}

static void
check_line_line_intersection_position (
    LineLineFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Check the intersection position for a particular line-line intersection.
   * Note that the LineLineFixture is reused here since the fixture doesn't
   * need additional data.  */

  PointType expected_pii
    = ((MarshalledPositionTestArgs *) marshalled_args)->expected_pii;

  PointType pii;   /* Position In Intersection */

  /* There are other tests that focus on whether the intersection test returns
   * the correct boolean value, hopefully they have been run first.  Should
   * this fail, it would be best to use those tests to investigate why.  */
  g_assert_true (
      LineLineIntersect (fixture->line1, fixture->line2, &pii) );

  /* WARNING: this test is fragile.  The expected_pii we compare to here is
   * hand-coded to match the particular (manually verified) results currently
   * given by LineLineIntersect() for the particular test input used, but
   * LineLineIntersect() only guarantees that the intersection point returned
   * in it's last argument is one of the points in the intersection, it doesn't
   * specify exactly which point, and the test data file used doesn't feature
   * an intersection containing only one point.  If this test itself breaks,
   * it will be necessary to manually verify and plug in the new values.  */
  g_assert_true (pii.X == expected_pii.X && pii.Y == expected_pii.Y);
}

static void
add_line_line_intersection_position_test (
    char *case_ending, PointType expected_pii )
{
  /* Add line-line intersection position test  */

  GString *testpath = g_string_new ("/line_line_intersection_position/");
  g_string_append (testpath, case_ending);

  GString *pcb_file
    = g_string_new ("test_data/line_line_intersection_position_");
  g_string_append (pcb_file, case_ending);
  g_string_append (pcb_file, ".pcb");

  MarshalledPositionTestArgs *ma = g_new (MarshalledPositionTestArgs, 1);
  ma->pcb_file = pcb_file->str;
  ma->expected_pii = expected_pii;

  g_test_add (
      testpath->str,
      LineLineFixture,
      ma,
      line_line_intersection_position_tests_fixture_setup,
      check_line_line_intersection_position,
      line_line_intersection_position_tests_fixture_tear_down );

  g_string_free (pcb_file, FALSE);  /* ->str itself is now owned by ma */
  g_string_free (testpath, TRUE);   /* g_test_add() copies testpath */
}

/* Add Line-Line Intersection Position Test */
#define ALLIPT(ce, epii) add_line_line_intersection_position_test(ce, (epii));

static void
line_arc_intersection_position_tests_fixture_setup (
    LineArcFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Analogous to line_line_intersection_position_fixture_setup() */
 
  char *pcb_file = ((MarshalledPositionTestArgs *) marshalled_args)->pcb_file;

  line_arc_intersection_tests_fixture_setup (fixture, pcb_file);
}

/* WARNING: despite the signature, we deallocate stuff pointed to by
 * marshaleld_args.  */
static void
line_arc_intersection_position_tests_fixture_tear_down (
    LineArcFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Analogous to line_line_intersection_position_tests_fixture_tear_down() */

  MarshalledPositionTestArgs *ma = (gpointer) marshalled_args;

  g_free (ma->pcb_file);
  g_free (ma);
}

static void
check_line_arc_intersection_position (
    LineArcFixture *fixture,
    gconstpointer marshalled_args )
{
  /* Analogous to check_line_line_intersection_position() */
  
  PointType expected_pii
    = ((MarshalledPositionTestArgs *) marshalled_args)->expected_pii;
  
  PointType pii;   /* Position In Intersection */

  g_assert_true (
      LineArcIntersect (fixture->the_line, fixture->the_arc, &pii) );

  /* WARNING: fragile.  See check_line_line_intersection_position().  */
  g_assert_true (pii.X == expected_pii.X && pii.Y == expected_pii.Y);
} 

static void
add_line_arc_intersection_position_test (
    char *case_ending, PointType expected_pii )
{
  /* Analogous to add_line_line_intersection_position_test() */

  GString *testpath = g_string_new ("/line_arc_intersection_position/");
  g_string_append (testpath, case_ending);

  GString *pcb_file
    = g_string_new ("test_data/line_arc_intersection_position_");
  g_string_append (pcb_file, case_ending);
  g_string_append (pcb_file, ".pcb");

  MarshalledPositionTestArgs *ma = g_new (MarshalledPositionTestArgs, 1);
  ma->pcb_file = pcb_file->str;
  ma->expected_pii = expected_pii;

  g_test_add (
      testpath->str,
      LineArcFixture,
      ma,
      line_arc_intersection_position_tests_fixture_setup,
      check_line_arc_intersection_position,
      line_arc_intersection_position_tests_fixture_tear_down );

  g_string_free (pcb_file, FALSE);  /* ->str itself is now owned by ma */
  g_string_free (testpath, TRUE);   /* g_test_add() copies testpath */
}

/* Add Line-Arc Intersection Position Test */
#define ALAIPT(ce, epii) add_line_arc_intersection_position_test(ce, (epii));

void
find_register_tests (void)
{
  /* Register all tests defined for this module for execution with g_test */

  /* Add line-line intersection test tests */
  ALLITT ("1", expect_line_line_intersection);
  ALLITT ("2", expect_line_line_intersection);
  ALLITT ("3", expect_line_line_intersection);
  ALLITT ("4", expect_line_line_intersection);
  ALLITT ("5", expect_no_line_line_intersection);
  ALLITT ("6", expect_no_line_line_intersection);
  ALLITT ("7", expect_no_line_line_intersection);
  ALLITT ("8", expect_no_line_line_intersection);

  /* Add line-arc intersection test tests */
  ALAITT ("1",  expect_line_arc_intersection);
  ALAITT ("2",  expect_line_arc_intersection);
  ALAITT ("3",  expect_line_arc_intersection);
  ALAITT ("4",  expect_line_arc_intersection);
  ALAITT ("5",  expect_line_arc_intersection);
  ALAITT ("6",  expect_line_arc_intersection);
  ALAITT ("7",  expect_line_arc_intersection);
  ALAITT ("8",  expect_line_arc_intersection);
  ALAITT ("9",  expect_line_arc_intersection);
  ALAITT ("10", expect_no_line_arc_intersection);
  ALAITT ("11", expect_no_line_arc_intersection);
  ALAITT ("12", expect_no_line_arc_intersection);
  ALAITT ("13", expect_no_line_arc_intersection);
  ALAITT ("14", expect_no_line_arc_intersection);
  
  /* Add pin-line intersection test tests */
  // FIXME: add a test that shows the fail in intersection tests for
  // octagonal pins?  Annoyingly, g_test doesn't look like is has xfail
  APLITT ("1", expect_pin_line_intersection);
  APLITT ("2", expect_pin_line_intersection);
  APLITT ("3", expect_pin_line_intersection);
  APLITT ("4", expect_pin_line_intersection);
  APLITT ("5", expect_no_pin_line_intersection);
  APLITT ("6", expect_no_pin_line_intersection);
  APLITT ("7", expect_no_pin_line_intersection);
  APLITT ("8", expect_no_pin_line_intersection);

  /* Add line-pad intersection test tests */
  ALPITT ("1",  expect_line_pad_intersection);
  ALPITT ("2",  expect_line_pad_intersection);
  ALPITT ("3",  expect_line_pad_intersection);
  ALPITT ("4",  expect_line_pad_intersection);
  ALPITT ("5",  expect_line_pad_intersection);
  ALPITT ("6",  expect_line_pad_intersection);
  ALPITT ("7",  expect_line_pad_intersection);
  ALPITT ("8",  expect_no_line_pad_intersection);
  ALPITT ("9",  expect_no_line_pad_intersection);
  ALPITT ("10", expect_no_line_pad_intersection);
  ALPITT ("11", expect_no_line_pad_intersection);
  ALPITT ("12", expect_line_pad_intersection);
  ALPITT ("13", expect_line_pad_intersection);
  ALPITT ("14", expect_line_pad_intersection);
  ALPITT ("15", expect_no_line_pad_intersection);
  ALPITT ("16", expect_no_line_pad_intersection);
  ALPITT ("17", expect_no_line_pad_intersection);
  
  /* Add arc-pad intersection test tests */
  /* IMPROVEME: these are somewhat random, should go through and more
   * carefully test different geometric arrangement, and arcs defined in both
   * directions.  */
  AAPITT ("1",  expect_arc_pad_intersection);
  AAPITT ("2",  expect_arc_pad_intersection);
  AAPITT ("3",  expect_arc_pad_intersection);
  AAPITT ("4",  expect_arc_pad_intersection);
  AAPITT ("5",  expect_arc_pad_intersection);
  AAPITT ("6",  expect_arc_pad_intersection);
  AAPITT ("7",  expect_arc_pad_intersection);
  AAPITT ("8",  expect_no_arc_pad_intersection);
  AAPITT ("9",  expect_no_arc_pad_intersection);
  AAPITT ("10", expect_no_arc_pad_intersection);
  AAPITT ("11", expect_no_arc_pad_intersection);
  AAPITT ("12", expect_arc_pad_intersection);
  AAPITT ("13", expect_arc_pad_intersection);
  AAPITT ("14", expect_arc_pad_intersection);
  AAPITT ("15", expect_no_arc_pad_intersection);
  AAPITT ("16", expect_no_arc_pad_intersection);
  AAPITT ("17", expect_no_arc_pad_intersection);

  /* These test are for "stubby" arcs, which are geometrically distinct from
   * the ones above in that they are so wide relative to their diameter that
   * they completely lack an inside edge -- the end cap circles overlap
   * instead.  Square pads are a convenient way to test this area because
   * they're pointy, so they can poke into the area of interest :)  */
  AAPITT ("18", expect_no_arc_pad_intersection);
  AAPITT ("19", expect_arc_pad_intersection);
  AAPITT ("20", expect_arc_pad_intersection);

  /* Add bloated line-line intersection test tests */
  ABLLITT ("1", expect_line_line_intersection);
  ABLLITT ("2", expect_line_line_intersection);
  ABLLITT ("3", expect_line_line_intersection);
  ABLLITT ("4", expect_no_line_line_intersection);
  ABLLITT ("5", expect_no_line_line_intersection);
  ABLLITT ("6", expect_no_line_line_intersection);

  /* Here we relay on symmetry and knowledge of the implementation instead of
   * explicitly testing line-line intersections under shrink.  */

  /* Add bloated line-arc intersection test tests.  Because the implementation
   * factors out test of (circular) end caps of both lines and arc, we don't
   * explicitly test the end points here: they have already been exercised by
   * the tests for line-line intersections under bloat, and the various
   * line-end point permutations have been tested for the non-bloated
   * condition.  */
  ABLAITT ("1", expect_line_arc_intersection);
  ABLAITT ("2", expect_line_arc_intersection);
  ABLAITT ("3", expect_line_arc_intersection);
  ABLAITT ("4", expect_no_line_arc_intersection);
  ABLAITT ("5", expect_no_line_arc_intersection);
  ABLAITT ("6", expect_no_line_arc_intersection);
  
  /* Here we relay on symmetry and knowledge of the implementation instead of
   * explicitly testing line-arc intersections under shrink.  */

  /* Pad intersection tests ultimately use lines for the pads, and pin/via
   * tests use circles (which have been tested a lot already, since they're
   * used at the ends of non-square lines). Therefore we now consider the
   * intersection tests to be pretty well exercised.  */
  
  /* Add line-line intersection position tests.  Only three are required,
   * because we really only need to check for line-line, circle-circle, and
   * line-circle correctness.  */
  ALLIPT ("1", ((PointType) { 6400000, 8100000 }));
  ALLIPT ("2", ((PointType) { 6382450, 5031707 }));
  ALLIPT ("3", ((PointType) { 6477000, 5800000 }));

  /* Add line-arc intersection position tests.  Only three are required,
   * because we really only need to check for circle-arc_outside,
   * circle-arce_inside, and line-arc_outside correctness.   */
  ALAIPT ("1", ((PointType) { 8637224, 10232595 }));
  ALAIPT ("2", ((PointType) { 5064990, 5277391 }));
  ALAIPT ("3", ((PointType) { 6747850, 8748956 }));
}

#endif
