/*
 *                            COPYRIGHT
 *
 *  PCB, interactive printed circuit board design
 *  Copyright (C) 1994,1995,1996 Thomas Nau
 *  Copyright (C) 1998,1999,2000,2001 harry eaton
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Contact addresses for paper mail and Email:
 *  harry eaton, 6697 Buttonhole Ct, Columbia, MD 21044 USA
 *  haceaton@aplcomm.jhuapl.edu
 *
 */

/*
 * This moduel, autoplace.c, was written by and is
 * Copyright (c) 2001 C. Scott Ananian
 */

/* functions used to autoplace elements.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <math.h>
#include <memory.h>
#include <stdlib.h>

#include "global.h"

#include "autoplace.h"
#include "box.h"
#include "data.h"
#include "draw.h"
#include "error.h"
#include "intersect.h"
#include "kdtree.h"
#include "macro.h"
#include "mirror.h"
#include "misc.h"
#include "move.h"
#include "mymem.h"
#include "rats.h"
#include "remove.h"
#include "rotate.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>  /* see http://dmalloc.com */
#endif

#define EXPANDRECTXY(r1, x1, y1, x2, y2) { \
  r1->X1=MIN(r1->X1, x1); r1->Y1=MIN(r1->Y1, y1); \
  r1->X2=MAX(r1->X2, x2); r1->Y2=MAX(r1->Y2, y2); \
}
#define EXPANDRECT(r1, r2) EXPANDRECTXY(r1, r2->X1, r2->Y1, r2->X2, r2->Y2)

/* ---------------------------------------------------------------------------
 * some local prototypes
 */
static double ComputeCost (NetListTypePtr Nets, double T0, double T);

/* ---------------------------------------------------------------------------
 * some local types
 */
const struct
{
  float via_cost;
  float congestion_penalty;	/* penalty length / unit area */
  float overlap_penalty_min;	/* penalty length / unit area at start */
  float overlap_penalty_max;	/* penalty length / unit area at end */
  float out_of_bounds_penalty;	/* assessed for each component oob */
  float overall_area_penalty;	/* penalty length / unit area */
  float matching_neighbor_bonus;	/* length bonus per same-type neigh. */
  float aligned_neighbor_bonus;	/* length bonus per aligned neigh. */
  float oriented_neighbor_bonus;	/* length bonus per same-rot neigh. */
#if 0
  float pin_alignment_bonus;	/* length bonus per exact alignment */
  float bound_alignment_bonus;	/* length bonus per exact alignment */
#endif
  float m;			/* annealing stage cutoff constant */
  float gamma;			/* annealing schedule constant */
  int good_ratio;		/* ratio of moves to good moves for halting */
  Boolean fast;			/* ignore SMD/pin conflicts */
  int large_grid_size;		/*snap perturbations to this grid when T is high */
  int small_grid_size;		/* snap to this grid when T is small. */
}
CostParameter =
{
  3e3,				/* via cost */
    2e-4,			/* congestion penalty */
    1e-0,			/* initial overlap penalty */
    1e5,			/* final overlap penalty */
    1e6,			/* out of bounds penalty */
    1e0,			/* penalty for total area used */
    1e3,			/* subtract 1000 from cost for every same-type neighbor */
    1e3,			/* subtract 1000 from cost for every aligned neighbor */
    1e3,			/* subtract 1000 from cost for every same-rotation neighbor */
    20,				/* move on when each module has been profitably moved 20 times */
    0.75,			/* annealing schedule constant: 0.85 */
    40,				/* halt when there are 60 times as many moves as good moves */
    False,			/* don't ignore SMD/pin conflicts */
    100,			/* coarse grid is 100 mils */
    10,				/* fine grid is 10 mils */
};

typedef struct
{
  ElementTypePtr *element;
  Cardinal elementN;
}
ElementPtrListType;

typedef struct
{
  ElementTypePtr element;
  enum
  { SHIFT, ROTATE, EXCHANGE }
  which;
  Position DX, DY;		/* for shift */
  BYTE rotate;			/* for rotate/flip */
  ElementTypePtr other;		/* for exchange */
}
PerturbationType;

/* ---------------------------------------------------------------------------
 * some local identifiers
 */

/* ---------------------------------------------------------------------------
 * Update the X, Y and group position information stored in the NetList after
 * elements have possibly been moved, rotated, flipped, etc.
 */
static void
UpdateXY (NetListTypePtr Nets)
{
  Cardinal SLayer, CLayer;
  Cardinal i, j;
  /* find layer groups of the component side and solder side */
  SLayer = GetLayerGroupNumberByNumber (MAX_LAYER + SOLDER_LAYER);
  CLayer = GetLayerGroupNumberByNumber (MAX_LAYER + COMPONENT_LAYER);
  /* update all nets */
  for (i = 0; i < Nets->NetN; i++)
    {
      for (j = 0; j < Nets->Net[i].ConnectionN; j++)
	{
	  ConnectionTypePtr c = &(Nets->Net[i].Connection[j]);
	  switch (c->type)
	    {
	    case PAD_TYPE:
	      c->group = TEST_FLAG (ONSOLDERFLAG,
				    (ElementTypePtr) c->ptr1)
		? SLayer : CLayer;
	      c->X = ((PadTypePtr) c->ptr2)->Point1.X;
	      c->Y = ((PadTypePtr) c->ptr2)->Point1.Y;
	      break;
	    case PIN_TYPE:
	      c->group = SLayer;	/* any layer will do */
	      c->X = ((PinTypePtr) c->ptr2)->X;
	      c->Y = ((PinTypePtr) c->ptr2)->Y;
	      break;
	    default:
	      Message ("Odd connection type encountered in " "UpdateXY");
	      break;
	    }
	}
    }
}

/* ---------------------------------------------------------------------------
 * Create a list of selected elements.
 */
static PointerListType
collectSelectedElements ()
{
  PointerListType list = { 0, 0, NULL };
  ELEMENT_LOOP (PCB->Data, if (TEST_FLAG (SELECTEDFLAG, element))
		{
		ElementTypePtr * epp = (ElementTypePtr *)
		GetPointerMemory (&list); *epp = element;}
  );
  return list;
}

#if 0				/* only for debugging box lists */
#include "create.h"
/* makes a line on the solder layer surrounding all boxes in blist */
static void
showboxes (BoxListTypePtr blist)
{
  Cardinal i;
  LayerTypePtr SLayer = &(PCB->Data->Layer[MAX_LAYER + SOLDER_LAYER]);
  for (i = 0; i < blist->BoxN; i++)
    {
      CreateNewLineOnLayer (SLayer, blist->Box[i].X1, blist->Box[i].Y1,
			    blist->Box[i].X2, blist->Box[i].Y1, 1, 1, 0);
      CreateNewLineOnLayer (SLayer, blist->Box[i].X1, blist->Box[i].Y2,
			    blist->Box[i].X2, blist->Box[i].Y2, 1, 1, 0);
      CreateNewLineOnLayer (SLayer, blist->Box[i].X1, blist->Box[i].Y1,
			    blist->Box[i].X1, blist->Box[i].Y2, 1, 1, 0);
      CreateNewLineOnLayer (SLayer, blist->Box[i].X2, blist->Box[i].Y1,
			    blist->Box[i].X2, blist->Box[i].Y2, 1, 1, 0);
    }
}
#endif

/* ---------------------------------------------------------------------------
 * Helper function to compute "closest neighbor" for a box in a kd-tree.
 * The closest neighbor on a certain side is the closest one in a trapezoid
 * emanating from that side.
 */
/*------ kd_find_neighbor ------*/
struct kd_neighbor_info
{
  const BoxType *neighbor;
  BoxType trap;
  direction_t search_dir;
};
#define ROTATEBOX(box) { Position t;\
    t = (box).X1; (box).X1 = - (box).Y1; (box).Y1 = t;\
    t = (box).X2; (box).X2 = - (box).Y2; (box).Y2 = t;\
    t = (box).X1; (box).X1 =   (box).X2; (box).X2 = t;\
}
/* helper methods for __kd_find_neighbor */
static int
__kd_find_neighbor_reg_in_sea (const BoxType * region, void *cl)
{
  struct kd_neighbor_info *ni = (struct kd_neighbor_info *) cl;
  BoxType query = *region;
  ROTATEBOX_TO_NORTH (query, ni->search_dir);
  /*  ______________ __ trap.y1     __
   *  \            /               |__| query rect.
   *   \__________/  __ trap.y2
   *   |          |
   *   trap.x1    trap.x2   sides at 45-degree angle
   */
  return (query.Y2 > ni->trap.Y1) && (query.Y1 < ni->trap.Y2) &&
    (query.X2 + ni->trap.Y2 > ni->trap.X1 + query.Y1) &&
    (query.X1 + query.Y1 < ni->trap.X2 + ni->trap.Y2);
}
static int
__kd_find_neighbor_rect_in_reg (const BoxType * box, void *cl)
{
  struct kd_neighbor_info *ni = (struct kd_neighbor_info *) cl;
  BoxType query = *box;
  int r;
  ROTATEBOX_TO_NORTH (query, ni->search_dir);
  /*  ______________ __ trap.y1     __
   *  \            /               |__| query rect.
   *   \__________/  __ trap.y2
   *   |          |
   *   trap.x1    trap.x2   sides at 45-degree angle
   */
  r = (query.Y2 > ni->trap.Y1) && (query.Y1 < ni->trap.Y2) &&
    (query.X2 + ni->trap.Y2 > ni->trap.X1 + query.Y1) &&
    (query.X1 + query.Y1 < ni->trap.X2 + ni->trap.Y2);
  r = r && (query.Y2 <= ni->trap.Y2);
  if (r)
    {
      ni->trap.Y1 = query.Y2;
      ni->neighbor = box;
    }
  return r;
}

/* main kd_find_neighbor routine.  Returns NULL if no neighbor in the
 * requested direction. */
static const BoxType *
kd_find_neighbor (kdtree_t * kdtree, const BoxType * box,
		  direction_t search_direction)
{
  struct kd_neighbor_info ni;
  BoxType bbox;

  ni.neighbor=NULL;
  ni.trap = *box;
  ni.search_dir = search_direction;

  bbox.X1 = bbox.Y1 = 0;
  bbox.X2 = PCB->MaxWidth;
  bbox.Y2 = PCB->MaxHeight;
  /* rotate so that we can use the 'north' case for everything */
  ROTATEBOX_TO_NORTH (bbox, search_direction);
  ROTATEBOX_TO_NORTH (ni.trap, search_direction);
  /* shift Y's such that trap contains full bounds of trapezoid */
  ni.trap.Y2 = ni.trap.Y1;
  ni.trap.Y1 = bbox.Y1;
  /* do the search! */
  kd_search (kdtree, NULL,
	     __kd_find_neighbor_reg_in_sea,
	     __kd_find_neighbor_rect_in_reg, &ni);
  return ni.neighbor;
}

/* ---------------------------------------------------------------------------
 * Compute cost function.
 *  note that area overlap cost is correct for SMD devices: SMD devices on
 *  opposite sides of the board don't overlap.
 *
 * Algorithms follow those described in sections 4.1 of
 *  "Placement and Routing of Electronic Modules" edited by Michael Pecht
 *  Marcel Dekker, Inc. 1993.  ISBN: 0-8247-8916-4 TK7868.P7.P57 1993
 */
static double
ComputeCost (NetListTypePtr Nets, double T0, double T)
{
  double W = 0;			/* wire cost */
  double delta1 = 0;		/* wire congestion penalty function */
  double delta2 = 0;		/* module overlap penalty function */
  double delta3 = 0;		/* out of bounds penalty */
  double delta4 = 0;		/* alignment bonus */
  double delta5 = 0;		/* total area penalty */
  Cardinal i, j;
  Position minx, maxx, miny, maxy;
  Boolean allpads, allsameside;
  Cardinal thegroup;
  BoxListType bounds = { 0, 0, NULL };	/* save bounding rectangles here */
  BoxListType solderside = { 0, 0, NULL };	/* solder side component bounds */
  BoxListType componentside = { 0, 0, NULL };	/* component side bounds */
  BoxListType thepins = { 0, 0, NULL };	/*pin list for alignment scoring */
  BoxListType thepads = { 0, 0, NULL };	/*pad list for alignment scoring */
  /* make sure the NetList have the proper updated X and Y coords */
  UpdateXY (Nets);
  /* wire length term.  approximated by half-perimeter of minimum
   * rectangle enclosing the net.  Note that we penalize vias in
   * all-SMD nets by making the rectangle a cube and weighting
   * the "layer height" of the net. */
  for (i = 0; i < Nets->NetN; i++)
    {
      NetTypePtr n = &Nets->Net[i];
      if (n->ConnectionN < 2)
	continue;		/* no cost to go nowhere */
      minx = maxx = n->Connection[0].X;
      miny = maxy = n->Connection[0].Y;
      thegroup = n->Connection[0].group;
      allpads = (n->Connection[0].type == PAD_TYPE);
      allsameside = True;
      for (j = 1; j < n->ConnectionN; j++)
	{
	  ConnectionTypePtr c = &(n->Connection[j]);
	  minx = MIN (minx, c->X);
	  maxx = MAX (maxx, c->X);
	  miny = MIN (miny, c->Y);
	  maxy = MAX (maxy, c->Y);
	  if (c->type != PAD_TYPE)
	    allpads = False;
	  if (c->group != thegroup)
	    allsameside = False;
	}
      /* save bounding rectangle */
      {
	BoxTypePtr box = GetBoxMemory (&bounds);
	box->X1 = minx;
	box->Y1 = miny;
	box->X2 = maxx;
	box->Y2 = maxy;
      }
      /* okay, add half-perimeter to cost! */
      W += (maxx - minx) + (maxy - miny) +
	((allpads && !allsameside) ? CostParameter.via_cost : 0);
    }
  /* now compute penalty function Wc which is proportional to
   * amount of overlap and congestion. */
  /* delta1 is congestion penalty function */
  delta1 = CostParameter.congestion_penalty *
    ComputeIntersectionArea (&bounds);
#if 0
  printf ("Wire Congestion Area: %f\n", ComputeIntersectionArea (&bounds));
#endif
  /* free bounding rectangles */
  FreeBoxListMemory (&bounds);
  /* now collect module areas (bounding rect of pins/pads) */
  /* two lists for solder side / component side. */
  ELEMENT_LOOP (PCB->Data,
		{
		BoxListTypePtr thisside;
		BoxListTypePtr otherside; BoxTypePtr box;
		BoxTypePtr lastbox = NULL; Dimension thickness;
		Dimension clearance; if (TEST_FLAG (ONSOLDERFLAG, element))
		{
		thisside = &solderside; otherside = &componentside;}
		else
		{
		thisside = &componentside; otherside = &solderside;}
		box = GetBoxMemory (thisside);
		/* protect against elements with no pins/pads */
		if (element->PinN == 0 && element->PadN == 0) continue;
		/* initialize box so that it will take the dimensions of
		 * the first pin/pad */
		box->X1 = PCB->MaxWidth; box->Y1 = PCB->MaxHeight;
		box->X2 = 0; box->Y2 = 0;
		PIN_LOOP (element,
			  thickness = pin->Thickness;
			  clearance = pin->Clearance;
			  EXPANDRECTXY (box,
					pin->X - (thickness / 2 +
						  2 * clearance),
					pin->Y - (thickness / 2 +
						  2 * clearance),
					pin->X + (thickness / 2 +
						  2 * clearance),
					pin->Y + (thickness / 2 +
						  2 * clearance)));
		PAD_LOOP (element, thickness = pad->Thickness;
			  clearance = pad->Clearance;
			  EXPANDRECTXY (box,
					MIN (pad->Point1.X,
					     pad->Point2.X) - (thickness / 2 +
							       2 * clearance),
					MIN (pad->Point1.Y,
					     pad->Point2.Y) - (thickness / 2 +
							       2 * clearance),
					MAX (pad->Point1.X,
					     pad->Point2.X) + (thickness / 2 +
							       2 * clearance),
					MAX (pad->Point1.Y,
					     pad->Point2.Y) + (thickness / 2 +
							       2 *
							       clearance)));
		/* add a box for each pin to the "opposite side":
		 * surface mount components can't sit on top of pins */
		if (!CostParameter.fast)
		PIN_LOOP (element,
			  box = GetBoxMemory (otherside);
			  thickness = pin->Thickness;
			  clearance = pin->Clearance;
			  /* we ignore clearance here */
			  /* (otherwise pins don't fit next to each other) */
			  box->X1 = pin->X - (thickness / 2);
			  box->Y1 = pin->Y - (thickness / 2);
			  box->X2 = pin->X + (thickness / 2);
			  box->Y2 = pin->Y + (thickness / 2);
			  /* speed hack! coalesce with last box if we can */
			  if (lastbox != NULL &&
			      ((lastbox->X1 == box->X1 &&
				lastbox->X2 == box->X2 &&
				MIN (abs (lastbox->Y1 - box->Y2),
				     abs (box->Y1 - lastbox->Y2)) <
				2 * clearance) || (lastbox->Y1 == box->Y1
						   && lastbox->Y2 == box->Y2
						   &&
						   MIN (abs
							(lastbox->X1 -
							 box->X2),
							abs (box->X1 -
							     lastbox->X2)) <
						   2 * clearance)))
			  {
			  EXPANDRECT (lastbox, box); otherside->BoxN--;}
			  else
			  lastbox = box;);
		/* assess out of bounds penalty */
		if (element->BoundingBox.X1 < 0 ||
		    element->BoundingBox.Y1 < 0 ||
		    element->BoundingBox.X2 >= PCB->MaxWidth ||
		    element->BoundingBox.Y2 >= PCB->MaxHeight)
		delta3 += CostParameter.out_of_bounds_penalty;
		/* heck, make our pin/pad lists while we're at it too */
		/* (this is for alignment scoring) */
		PIN_LOOP (element,
			  box = GetBoxMemory (&thepins);
			  box->X1 = box->X2 = pin->X;
			  box->Y1 = box->Y2 = pin->Y;);
		PAD_LOOP (element,
			  box = GetBoxMemory (&thepads);
			  box->X1 = box->X2 = pad->Point1.X;
			  box->Y1 = box->Y2 = pad->Point1.Y;);}
  );
  /* compute intersection area of module areas box list */
  delta2 = (ComputeIntersectionArea (&solderside) +
	    ComputeIntersectionArea (&componentside)) *
    (CostParameter.overlap_penalty_min +
     (1 - (T / T0)) * CostParameter.overlap_penalty_max);
#if 0
  printf ("Module Overlap Area (solder): %f\n",
	  ComputeIntersectionArea (&solderside));
  printf ("Module Overlap Area (component): %f\n",
	  ComputeIntersectionArea (&componentside));
#endif
  FreeBoxListMemory (&solderside);
  FreeBoxListMemory (&componentside);
  /* reward pin/pad x/y alignment */
  /* score higher if pins/pads belong to same *type* of component */
  /* XXX: subkey should be *distance* from thing aligned with, so that
   * aligning to something far away isn't profitable */
  {
    /* create k-d tree */
    PointerListType seboxes = { 0, 0, NULL }
    , ceboxes =
    {
    0, 0, NULL};
    struct ebox
    {
      BoxType box;
      ElementTypePtr element;
    };
    direction_t dir[4] = { NORTH, EAST, SOUTH, WEST };
    struct ebox **boxpp, *boxp;
    kdtree_t *kdt_s, *kdt_c;
    int factor;
    ELEMENT_LOOP (PCB->Data,
		  boxpp = (struct ebox **)
		  GetPointerMemory (TEST_FLAG (ONSOLDERFLAG, element) ?
				    &seboxes : &ceboxes);
		  *boxpp = malloc (sizeof (**boxpp));
		  (*boxpp)->box = element->BoundingBox;
		  (*boxpp)->element = element;);
    kdt_s = kd_create_tree ((const BoxType **) seboxes.Ptr, seboxes.PtrN, 1);
    kdt_c = kd_create_tree ((const BoxType **) ceboxes.Ptr, ceboxes.PtrN, 1);
    FreePointerListMemory (&seboxes);
    FreePointerListMemory (&ceboxes);
    /* now, for each element, find its neighbor on all four sides */
    delta4 = 0;
    for (i = 0; i < 4; i++)
      ELEMENT_LOOP (PCB->Data,
		    boxp = (struct ebox *)
		    kd_find_neighbor (TEST_FLAG (ONSOLDERFLAG, element) ?
				      kdt_s : kdt_c,
				      &element->BoundingBox, dir[i]);
		    /* score bounding box alignments */
		    if (!boxp) continue;
		    factor = 1;
		    if (0 == strcmp (element->Name[0].TextString,
				     boxp->element->Name[0].TextString))
		    {
		    delta4 += CostParameter.matching_neighbor_bonus; factor++;}
		    if (element->Name[0].Direction ==
			boxp->element->Name[0].Direction)
		    delta4 += factor * CostParameter.oriented_neighbor_bonus;
		    if (element->BoundingBox.X1 ==
			boxp->element->BoundingBox.X1 ||
			element->BoundingBox.X1 ==
			boxp->element->BoundingBox.X2 ||
			element->BoundingBox.X2 ==
			boxp->element->BoundingBox.X1 ||
			element->BoundingBox.X2 ==
			boxp->element->BoundingBox.X2 ||
			element->BoundingBox.Y1 ==
			boxp->element->BoundingBox.Y1 ||
			element->BoundingBox.Y1 ==
			boxp->element->BoundingBox.Y2 ||
			element->BoundingBox.Y2 ==
			boxp->element->BoundingBox.Y1 ||
			element->BoundingBox.Y2 ==
			boxp->element->BoundingBox.Y2)
		    delta4 += factor * CostParameter.aligned_neighbor_bonus;);
    /* free k-d tree memory */
    kd_destroy_tree (&kdt_s);
    kd_destroy_tree (&kdt_c);
  }
  /* penalize total area used by this layout */
  {
    Position minX = PCB->MaxWidth, minY = PCB->MaxHeight;
    Position maxX = 0, maxY = 0;
    ELEMENT_LOOP (PCB->Data,
		  minX = MIN (minX, element->BoundingBox.X1);
		  minY = MIN (minY, element->BoundingBox.Y1);
		  maxX = MAX (maxX, element->BoundingBox.X2);
		  maxY = MAX (maxY, element->BoundingBox.Y2););
    if (minX < maxX && minY < maxY)
      delta5 = CostParameter.overall_area_penalty *
	(maxX - minX) * (maxY - minY);
  }
  /* done! */
  return W + (delta1 + delta2 + delta3 - delta4 + delta5);
}

/* ---------------------------------------------------------------------------
 * Perturb:
 *  1) flip SMD from solder side to component side or vice-versa.
 *  2) rotate component 90, 180, or 270 degrees.
 *  3) shift component random + or - amount in random direction.
 *     (magnitude of shift decreases over time)
 *  -- Only perturb selected elements (need count/list of selected?) --
 */
PerturbationType
createPerturbation (PointerListTypePtr selected, double T)
{
  PerturbationType pt;
  /* pick element to perturb */
  pt.element = (ElementTypePtr) selected->Ptr[random () % selected->PtrN];
  /* exchange, flip/rotate or shift? */
  switch (random () % ((selected->PtrN > 1) ? 3 : 2))
    {
    case 0:
      {				/* shift! */
	int grid;
	double scaleX = MAX (250, MIN (sqrt (T), PCB->MaxWidth / 3));
	double scaleY = MAX (250, MIN (sqrt (T), PCB->MaxHeight / 3));
	pt.which = SHIFT;
	pt.DX = scaleX * 2 * ((((double) random ()) / RAND_MAX) - 0.5);
	pt.DY = scaleY * 2 * ((((double) random ()) / RAND_MAX) - 0.5);
	/* snap to grid. different grids for "high" and "low" T */
	grid = (T > 1000) ? CostParameter.large_grid_size :
	  CostParameter.small_grid_size;
	/* (round away from zero) */
	pt.DX = ((pt.DX / grid) + SGN (pt.DX)) * grid;
	pt.DY = ((pt.DY / grid) + SGN (pt.DY)) * grid;
	/* limit DX/DY so we don't fall off board */
	pt.DX = MAX (pt.DX, -pt.element->BoundingBox.X1);
	pt.DX = MIN (pt.DX, PCB->MaxWidth - pt.element->BoundingBox.X2);
	pt.DY = MAX (pt.DY, -pt.element->BoundingBox.Y1);
	pt.DY = MIN (pt.DY, PCB->MaxHeight - pt.element->BoundingBox.Y2);
	/* all done but the movin' */
	break;
      }
    case 1:
      {				/* flip/rotate! */
	/* only flip if it's an SMD component */
	Boolean isSMD = pt.element->PadN != 0;
	pt.which = ROTATE;
	pt.rotate = isSMD ? (random () & 3) : (1 + (random () % 3));
	/* 0 - flip; 1-3, rotate. */
	break;
      }
    case 2:
      {				/* exchange! */
	pt.which = EXCHANGE;
	pt.other = (ElementTypePtr)
	  selected->Ptr[random () % (selected->PtrN - 1)];
	if (pt.other == pt.element)
	  pt.other = (ElementTypePtr) selected->Ptr[selected->PtrN - 1];
	/* don't allow exchanging a solderside-side SMD component
	 * with a non-SMD component. */
	if ((pt.element->PinN != 0 /* non-SMD */  &&
	     TEST_FLAG (ONSOLDERFLAG, pt.other)) ||
	    (pt.other->PinN != 0 /* non-SMD */  &&
	     TEST_FLAG (ONSOLDERFLAG, pt.element)))
	  return createPerturbation (selected, T);
	break;
      }
    default:
      assert (0);
    }
  return pt;
}

void
doPerturb (PerturbationType * pt, Boolean undo)
{
  Position bbcx, bbcy;
  /* compute center of element bounding box */
  bbcx = (pt->element->BoundingBox.X1 + pt->element->BoundingBox.X2) / 2;
  bbcy = (pt->element->BoundingBox.Y1 + pt->element->BoundingBox.Y2) / 2;
  /* do exchange, shift or flip/rotate */
  switch (pt->which)
    {
    case SHIFT:
      {
	Position DX = pt->DX, DY = pt->DY;
	if (undo)
	  {
	    DX = -DX;
	    DY = -DY;
	  }
	MoveElementLowLevel (pt->element, DX, DY);
	return;
      }
    case ROTATE:
      {
	BYTE b = pt->rotate;
	if (undo)
	  b = (4 - b) & 3;
	/* 0 - flip; 1-3, rotate. */
	if (b)
	  RotateElementLowLevel (pt->element, bbcx, bbcy, b);
	else
	  {
	    Position y = pt->element->BoundingBox.Y1;
	    MirrorElementCoordinates (pt->element, 0);
	    /* mirroring moves the element.  move it back. */
	    MoveElementLowLevel (pt->element, 0,
				 y - pt->element->BoundingBox.Y1);
	  }
	return;
      }
    case EXCHANGE:
      {
	/* first exchange positions */
	Position x1 = pt->element->BoundingBox.X1;
	Position y1 = pt->element->BoundingBox.Y1;
	Position x2 = pt->other->BoundingBox.X1;
	Position y2 = pt->other->BoundingBox.Y1;
	MoveElementLowLevel (pt->element, x2 - x1, y2 - y1);
	MoveElementLowLevel (pt->other, x1 - x2, y1 - y2);
	/* then flip both elements if they are on opposite sides */
	if (TEST_FLAG (ONSOLDERFLAG, pt->element) !=
	    TEST_FLAG (ONSOLDERFLAG, pt->other))
	  {
	    PerturbationType mypt;
	    mypt.element = pt->element;
	    mypt.which = ROTATE;
	    mypt.rotate = 0;	/* flip */
	    doPerturb (&mypt, undo);
	    mypt.element = pt->other;
	    doPerturb (&mypt, undo);
	  }
	/* done */
	return;
      }
    default:
      assert (0);
    }
}

/* ---------------------------------------------------------------------------
 * Auto-place selected components.
 */
Boolean
AutoPlaceSelected (void)
{
  NetListTypePtr Nets;
  PointerListType Selected;
  PerturbationType pt;
  double C0, T0;
  Boolean changed = False;

  /* (initial netlist processing copied from AddAllRats) */
  /* the netlist library has the text form
   * ProcNetlist fills in the Netlist
   * structure the way the final routing
   * is supposed to look
   */
  Nets = ProcNetlist (&PCB->NetlistLib);
  if (!Nets)
    {
      Message ("Can't add rat lines because no netlist is loaded.\n");
      goto done;
    }

  Selected = collectSelectedElements ();
  if (Selected.PtrN == 0)
    {
      Message ("No elements selected to autoplace.\n");
      goto done;
    }

  /* simulated annealing */
  {				/* compute T0 by doing a random series of moves. */
    const int TRIALS = 10;
    const double Tx = 3e5, P = 0.95;
    double Cs = 0.0;
    int i;
    C0 = ComputeCost (Nets, Tx, Tx);
    for (i = 0; i < TRIALS; i++)
      {
	pt = createPerturbation (&Selected, 1e6);
	doPerturb (&pt, False);
	Cs += fabs (ComputeCost (Nets, Tx, Tx) - C0);
	doPerturb (&pt, True);
      }
    T0 = -(Cs / TRIALS) / log (P);
    printf ("Initial T: %f\n", T0);
  }
  /* now anneal in earnest */
  {
    double T = T0;
    long steps = 0;
    int good_moves = 0, moves = 0;
    const int good_move_cutoff = CostParameter.m * Selected.PtrN;
    const int move_cutoff = 2 * good_move_cutoff;
    C0 = ComputeCost (Nets, T0, T);
    while (1)
      {
	double Cprime;
	pt = createPerturbation (&Selected, T);
	doPerturb (&pt, False);
	Cprime = ComputeCost (Nets, T0, T);
	if (Cprime < C0)
	  {			/* good move! */
	    C0 = Cprime;
	    good_moves++;
	    steps++;
	  }
	else if ((random () / (double) RAND_MAX) < exp ((C0 - Cprime) / T))
	  {
	    /* not good but keep it anyway */
	    C0 = Cprime;
	    steps++;
	  }
	else
	  doPerturb (&pt, True);	/* undo last change */
	moves++;
	/* are we at the end of a stage? */
	if (good_moves >= good_move_cutoff || moves >= move_cutoff)
	  {
	    printf ("END OF STAGE: COST %.0f\t"
		    "GOOD_MOVES %d\tMOVES %d\t"
		    "T: %.1f\n", C0, good_moves, moves, T);
	    /* is this the end? */
	    if (T < 5 || good_moves < moves / CostParameter.good_ratio)
	      break;
	    /* nope, adjust T and continue */
	    moves = good_moves = 0;
	    T *= CostParameter.gamma;
	    /* cost is T dependent, so recompute */
	    C0 = ComputeCost (Nets, T0, T);
	  }
      }
    changed = (steps > 0);
  }
done:
  if (changed)
    {
      DeleteRats (False);
      AddAllRats (False, NULL);
      ClearAndRedrawOutput ();
    }
  FreePointerListMemory (&Selected);
  return (changed);
}
