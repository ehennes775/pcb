/*!
 * \file src/snap.h
 *
 * \brief Snapping structures and functions.
 *
 * <hr>
 *
 * <h1><b>Copyright.</b></h1>\n
 *
 * PCB, interactive printed circuit board design
 *
 * Copyright (C) 2017 Charles Parker
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

#ifndef snap_h
#define snap_h

#include <stdio.h>
#include <stdbool.h>
#include "global.h"  /* type definitions */

/*
 * Snap Type and functions
 */

struct SnapSpecStruct;
typedef struct SnapSpecStruct SnapSpecType;

/*!
 * A structure that contains information about an object that is the result of a
   snap search.
 
   This should probably include a pointer to the object that was found. If it
   did then there may be no need for the obj_type. The object would also know
   where it is, but the snap point might not actually be where the object it, 
   for example, if we're snapping to a point along a line.
 */
typedef struct
{
  /*! A pointer to the SnapSpec that this came from. */
  SnapSpecType * spec;
  /*! A bool to indicate that an object was found. */
  bool valid;
  /*! The point to snap to. */
  PointType loc;
  /*! The square of the distance from the mouse pointer to the snap point. */
  Coord distsq;
  /*! The type of the object that was found. */
  unsigned obj_type;
} SnapType;

/*
 * Snap Spec Type and functions
 */

/*
 * A SnapSpec is a structure that contains information about snapping to
 * something. The search function is called to look for something.
 */
struct SnapSpecStruct
{
  /*! The name of the snap.*/
  char * name;
  //! A function to search for objects to snap to
  /*!
   There is a question in my mind about what the call signature for this
   function should look like. Should it take the radius like it currently does?
   Or should it take a pointer to the snap spec? Presently the function has no
   way to know anything about anything outside of itself.
   \param x x coordinate to search
   \param y y coordinate to search
   \param r radius for the search
   \return A SnapType object containing the result of the search
   */
  SnapType (*search)(Coord x, Coord y, Coord r);
  //! Enable flag
  /*! A flag to indicate that the search should be executed or skipped. */
  bool enabled;
  //! Snapping priority
  /*! Objects are searched for in priority order. Things with a higher priority
      are searched for first. */
  int priority;
  //! Search radius
  /*! The maximum distance away from the pointer that the search function should
      look for objects. */
  Coord radius;
  //! The type of object this spec searches for.
  int obj_type;
};

/* how many parameters ought to be specified in this constructor? 
   For insertion into a snap list it needs at least a name and a priority,
   so I'll go with that for now. */

/*! \brief SnapSpec constructor 
    \param name The name of the SnapSpec
    \param priority The priority of the snapping.
    \return The new SnapSpec structure.
    \sa SnapSpecStruct
 */
SnapSpecType * snap_spec_new(char * name, int priority);
/*! \brief SnapSpec destructor
 *  \param snap The snap to free from memory.
 */
void snap_spec_delete(SnapSpecType * snap);

/*
 * Snap List Type and functions
 */

/*! \brief A structure containing an ordered list of SnapSpecs.
 
    This structure implements a list of SnapSpecs. When an element is added, a
    copy is made in the memory of the structure. This is done in order to keep
    all of the objects to be iterated over in the same region of memory in order
    to keep the iteration fast. The iteration must be fast since the list may be
    iterated through every time the mouse is moved.
 
    When SnapSpecs are added, they are inserted into the list at a position
    according to their priority. The memory is always kept contiguous, so the
    elements after are all shifted down in memory to make room for the new one.
 
    If an element is subsequently removed, the size of the allocated memory is
    not reduced, but the memory is still held. The elements in the list after 
    the one removed are all shifted in memory so that all the elements in the
    list remain contiguous. When the element is removed, \a n is decremented,
    but max is not. Another snap can be added and fill that memory.
 */
typedef struct {
  int n,   /*! The number of elements in the list. */
      max; /*! The maximum number of elements the list can presently hold. */
  SnapSpecType * snaps; /*! The list of snaps. */
} SnapListType;

/* constructor */
SnapListType * snap_list_new();
/* destructor */
void snap_list_delete(SnapListType * list);

SnapSpecType * snap_list_add_snap(SnapListType * list, SnapSpecType * snap);
int snap_list_remove_snap_by_name(SnapListType * list, char * name);
SnapSpecType * snap_list_find_snap_by_name(SnapListType * list, char * name);
int snap_list_list_snaps(SnapListType * list);
SnapType * snap_list_search_snaps(SnapListType * list, Coord x, Coord y);



#endif /* snap_h */
