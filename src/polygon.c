/* $Id$ */

/*
 *                            COPYRIGHT
 *
 *  PCB, interactive printed circuit board design
 *  Copyright (C) 1994,1995,1996 Thomas Nau
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
 *  Thomas Nau, Schlehenweg 15, 88471 Baustetten, Germany
 *  Thomas.Nau@rz.uni-ulm.de
 *
 */


/* special polygon editing routines
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <math.h>
#include <memory.h>
#include <setjmp.h>

#include "global.h"
#include "box.h"
#include "create.h"
#include "crosshair.h"
#include "data.h"
#include "draw.h"
#include "error.h"
#include "find.h"
#include "misc.h"
#include "move.h"
#include "polygon.h"
#include "remove.h"
#include "rtree.h"
#include "search.h"
#include "set.h"
#include "thermal.h"
#include "undo.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif

RCSID ("$Id$");

#define ROUND(x) ((long)(((x) >= 0 ? (x) + 0.5  : (x) - 0.5)))

/* ---------------------------------------------------------------------------
 * local prototypes
 */

#define CIRC_SEGS 36
static double circleVerticies[(CIRC_SEGS / 4 + 1) * 2] = {
  1.0, 0.0,
  0.98480775301221, 0.17364817766693,
  0.93969262978591, 0.34202014332567,
  0.86602540478444, 0.5,
  0.76604444311898, 0.64278760968654,
  0.64278760968654, 0.76604444311898,
  0.5, 0.86602540478444,
  0.34202014332567, 0.93969262978591,
  0.17364817766693, 0.98480775301221,
  0.0, 1.0
};


static POLYAREA *
biggest (POLYAREA * p)
{
  POLYAREA *n, *top = NULL;
  PLINE *pl;
  double big = 0;
  if (!p)
    return NULL;
  n = p;
  do
    {
      if (n->contours->area > big)
	{
	  top = n;
	  big = n->contours->area;
	}
    }
  while ((n = n->f) != p);
  assert (top);
  if (top == p)
    return p;
  pl = top->contours;
  top->contours = p->contours;
  p->contours = pl;
  assert (pl);
  assert (p->f);
  assert (p->b);
  return p;
}

POLYAREA *
ContourToPoly (PLINE * contour)
{
  POLYAREA *p;
  poly_PreContour (contour, TRUE);
  assert (contour->Flags.orient == PLF_DIR);
  if (!(p = poly_Create ()))
    return NULL;
  poly_InclContour (p, contour);
  assert (poly_Valid (p));
  return p;
}

static POLYAREA *
original_poly (PolygonType * p)
{
  PLINE *contour = NULL;
  POLYAREA *np = NULL;
  Vector v;

  /* first make initial polygon contour */
  POLYGONPOINT_LOOP (p);
  {
    v[0] = point->X;
    v[1] = point->Y;
    if (contour == NULL)
      {
	if ((contour = poly_NewContour (v)) == NULL)
	  return NULL;
      }
    else
      {
	poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      }
  }
  END_LOOP;
  poly_PreContour (contour, TRUE);
  /* make sure it is a positive contour */
  if ((contour->Flags.orient) != PLF_DIR)
    poly_InvContour (contour);
  assert ((contour->Flags.orient) == PLF_DIR);
  if ((np = poly_Create ()) == NULL)
    return NULL;
  poly_InclContour (np, contour);
  assert (poly_Valid (np));
  return biggest (np);
}

static int
ClipOriginal (PolygonType * poly)
{
  POLYAREA *p, *result;
  int r;

  p = original_poly (poly);
  r = poly_Boolean_free (poly->Clipped, p, &result, PBO_ISECT);
  if (r != err_ok)
    {
      fprintf (stderr, "Error while clipping PBO_ISECT\n");
      poly_Free (&result);
      poly->Clipped = NULL;
      return 0;
    }
  poly->Clipped = biggest (result);
  assert (!poly->Clipped || poly_Valid (poly->Clipped));
  return 1;
}

POLYAREA *
RectPoly (LocationType x1, LocationType x2, LocationType y1, LocationType y2)
{
  PLINE *contour = NULL;
  Vector v;

  assert (x2 > x1);
  assert (y2 > y1);
  v[0] = x1;
  v[1] = y1;
  if ((contour = poly_NewContour (v)) == NULL)
    return NULL;
  v[0] = x2;
  v[1] = y1;
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x2;
  v[1] = y2;
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x1;
  v[1] = y2;
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  return ContourToPoly (contour);
}

POLYAREA *
OctagonPoly (LocationType x, LocationType y, BDimension radius)
{
  PLINE *contour = NULL;
  Vector v;

  v[0] = x + ROUND (radius * 0.5);
  v[1] = y + ROUND (radius * TAN_22_5_DEGREE_2);
  if ((contour = poly_NewContour (v)) == NULL)
    return NULL;
  v[0] = x + ROUND (radius * TAN_22_5_DEGREE_2);
  v[1] = y + ROUND (radius * 0.5);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x - (v[0] - x);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x - ROUND (radius * 0.5);
  v[1] = y + ROUND (radius * TAN_22_5_DEGREE_2);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[1] = y - (v[1] - y);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x - ROUND (radius * TAN_22_5_DEGREE_2);
  v[1] = y - ROUND (radius * 0.5);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x - (v[0] - x);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = x + ROUND (radius * 0.5);
  v[1] = y - ROUND (radius * TAN_22_5_DEGREE_2);
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  return ContourToPoly (contour);
}

/* add verticies in a half-circle starting from v 
 * centered at X, Y and going counter-clockwise
 * does not include the last point in the half circle
 */
void
half_circle (PLINE * c, LocationType X, LocationType Y, Vector v)
{
  double e1, e2, t1;
  int i;

  poly_InclVertex (c->head.prev, poly_CreateNode (v));
  /* move vector to origin */
  e1 = v[0] - X;
  e2 = v[1] - Y;
  for (i = 0; i < (CIRC_SEGS - 1) / 2; i++)
    {
      /* rotate the vector */
      t1 = e1 * circleVerticies[2] - e2 * circleVerticies[3];
      e2 = e1 * circleVerticies[3] + e2 * circleVerticies[2];
      e1 = t1;
      v[0] = X + ROUND (e1);
      v[1] = Y + ROUND (e2);
      poly_InclVertex (c->head.prev, poly_CreateNode (v));
    }
}

#define COARSE_CIRCLE 0
/* create a 35-vertex circle approximation */
POLYAREA *
CirclePoly (LocationType x, LocationType y, BDimension radius)
{
  PLINE *contour;
  Vector v;
  int i;

  if (radius <= 0)
    return NULL;
  v[0] = x + radius;
  v[1] = y;
  if ((contour = poly_NewContour (v)) == NULL)
    return NULL;
  for (i = 2; i < 20;)
    {
      v[0] = x + circleVerticies[i++] * radius;
      v[1] = y + circleVerticies[i++] * radius;
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      i += COARSE_CIRCLE;
    }
  for (i = 17; i > 0;)
    {
      v[1] = y + circleVerticies[i--] * radius;
      v[0] = x - circleVerticies[i--] * radius;
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      i -= COARSE_CIRCLE;
    }
  for (i = 2; i < 20;)
    {
      v[0] = x - circleVerticies[i++] * radius;
      v[1] = y - circleVerticies[i++] * radius;
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      i += COARSE_CIRCLE;
    }
  for (i = 17; i > 2;)
    {
      v[1] = y - circleVerticies[i--] * radius;
      v[0] = x + circleVerticies[i--] * radius;
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      i -= COARSE_CIRCLE;;
    }
  return ContourToPoly (contour);
}

#define ARC_ANGLE 5
POLYAREA *
ArcPoly (ArcType * a, BDimension thick)
{
  PLINE *contour = NULL;
  POLYAREA *np = NULL;
  Vector v;
  BoxType *ends;
  int i, segs;
  double ang, da, rx, ry;
  long half;

  if (thick <= 0)
    return NULL;
  if (a->Delta < 0)
    {
      a->StartAngle += a->Delta;
      a->Delta = -a->Delta;
    }
  half = (thick + 1) / 2;
  ends = GetArcEnds (a);
  /* start with inner radius */
  rx = MAX (a->Width - half, 0);
  ry = MAX (a->Height - half, 0);
  segs = a->Delta / ARC_ANGLE;
  ang = a->StartAngle;
  da = (1.0 * a->Delta) / segs;
  v[0] = a->X - rx * cos (ang * M180);
  v[1] = a->Y + ry * sin (ang * M180);
  if ((contour = poly_NewContour (v)) == NULL)
    return 0;
  for (i = 0; i < segs - 1; i++)
    {
      ang += da;
      v[0] = a->X - rx * cos (ang * M180);
      v[1] = a->Y + ry * sin (ang * M180);
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
    }
  /* find last point */
  ang = a->StartAngle + a->Delta;
  v[0] = a->X - rx * cos (ang * M180);
  v[1] = a->Y + ry * sin (ang * M180);
  /* add the round cap at the end */
  half_circle (contour, ends->X2, ends->Y2, v);
  /* and now do the outer arc (going backwards) */
  rx = a->Width + half;
  ry = a->Width + half;
  da = -da;
  for (i = 0; i < segs; i++)
    {
      v[0] = a->X - rx * cos (ang * M180);
      v[1] = a->Y + ry * sin (ang * M180);
      poly_InclVertex (contour->head.prev, poly_CreateNode (v));
      ang += da;
    }
  /* now add other round cap */
  ang = a->StartAngle;
  v[0] = a->X - rx * cos (ang * M180);
  v[1] = a->Y + ry * sin (ang * M180);
  half_circle (contour, ends->X1, ends->Y1, v);
  /* now we have the whole contour */
  if (!(np = ContourToPoly (contour)))
    return NULL;
  return np;
}

POLYAREA *
LinePoly (LineType * l, BDimension thick)
{
  PLINE *contour = NULL;
  POLYAREA *np = NULL;
  Vector v;
  double d, dx, dy;
  long half;

  if (thick <= 0)
    return NULL;
  half = (thick + 1) / 2;
  d =
    sqrt (SQUARE (l->Point1.X - l->Point2.X) +
	  SQUARE (l->Point1.Y - l->Point2.Y));
  if (d == 0)			/* line is a point */
    return CirclePoly (l->Point1.X, l->Point1.Y, half);
  d = half / d;
  dx = (l->Point1.Y - l->Point2.Y) * d;
  dy = (l->Point2.X - l->Point1.X) * d;
  v[0] = l->Point1.X - dx;
  v[1] = l->Point1.Y - dy;
  if ((contour = poly_NewContour (v)) == NULL)
    return 0;
  v[0] = l->Point2.X - dx;
  v[1] = l->Point2.Y - dy;
  half_circle (contour, l->Point2.X, l->Point2.Y, v);
  v[0] = l->Point2.X + dx;
  v[1] = l->Point2.Y + dy;
  poly_InclVertex (contour->head.prev, poly_CreateNode (v));
  v[0] = l->Point1.X + dx;
  v[1] = l->Point1.Y + dy;
  half_circle (contour, l->Point1.X, l->Point1.Y, v);
  /* now we have the line contour */
  if (!(np = ContourToPoly (contour)))
    return NULL;
  return np;
}

/* clear np1 from the polygon */
static int
Subtract (POLYAREA * np1, PolygonType * p, Boolean fnp)
{
  POLYAREA *merged = NULL, *np = np1;;
  int x;
  assert (np);
  assert (p);
  if (!p->Clipped)
    {
      if (fnp)
        poly_Free (&np);
      return 1;
    }
  assert (poly_Valid (p->Clipped));
  assert (poly_Valid (np));
  if (fnp)
  x = poly_Boolean_free (p->Clipped, np, &merged, PBO_SUB);
  else
  {
  x = poly_Boolean (p->Clipped, np, &merged, PBO_SUB);
  poly_Free (&p->Clipped);
  }
  assert (!merged || poly_Valid (merged));
  if (x != err_ok)
    {
      fprintf (stderr, "Error while clipping PBO_SUB\n");
      poly_Free (&merged);
      p->Clipped = NULL;
      return 0;
    }
  p->Clipped = biggest (merged);
  assert (!p->Clipped || poly_Valid (p->Clipped));
  if (!p->Clipped)
    Message ("Polygon cleared out of existence near (%d, %d)\n",
	     (p->BoundingBox.X1 + p->BoundingBox.X2) / 2,
	     (p->BoundingBox.Y1 + p->BoundingBox.Y2) / 2);
  return 1;
}

/* create a polygon of the pin clearance */
POLYAREA *
PinPoly (PinType * pin, BDimension thick)
{
  int size = (thick + 1) / 2;
  if (TEST_FLAG (SQUAREFLAG, pin))
    {
      return RectPoly (pin->X - size, pin->X + size, pin->Y - size,
		       pin->Y + size);
    }
  else if (TEST_FLAG (OCTAGONFLAG, pin))
    {
      return OctagonPoly (pin->X, pin->Y, size + size);
    }
  return CirclePoly (pin->X, pin->Y, size);
}

/* remove the pin clearance from the polygon */
static int
SubtractPin (DataType * d, PinType * pin, LayerType * l, PolygonType * p)
{
  POLYAREA *np;
  Cardinal i;

  if (pin->Clearance == 0)
    return 0;
  i = GetLayerNumber (d, l);
  if (TEST_THERM (i, pin))
    np = ThermPoly (pin, i);
  else
    np = PinPoly (pin, pin->Thickness + pin->Clearance);
  if (!np)
    return 0;
  return Subtract (np, p, TRUE); //!TEST_THERM (i, pin));
}

static int
SubtractLine (LineType * line, PolygonType * p)
{
  POLYAREA *np;

  if (!TEST_FLAG (CLEARLINEFLAG, line))
    return 0;
  if (!(np = LinePoly (line, line->Thickness + line->Clearance)))
    return 0;
  return Subtract (np, p, True);
}

static int
SubtractArc (ArcType * arc, PolygonType * p)
{
  POLYAREA *np;

  if (!TEST_FLAG (CLEARLINEFLAG, arc))
    return 0;
  if (!(np = ArcPoly (arc, arc->Thickness + arc->Clearance)))
    return 0;
  return Subtract (np, p, True);
}

static int
SubtractPad (PadType * pad, PolygonType * p)
{
  POLYAREA *np = NULL;

  if (TEST_FLAG (SQUAREFLAG, pad))
    {
      BDimension t = (pad->Thickness + pad->Clearance) / 2;
      LocationType x1, x2, y1, y2;
      x1 = MIN (pad->Point1.X, pad->Point2.X) - t;
      x2 = MAX (pad->Point1.X, pad->Point2.X) + t;
      y1 = MIN (pad->Point1.Y, pad->Point2.Y) - t;
      y2 = MAX (pad->Point1.Y, pad->Point2.Y) + t;
      if (!(np = RectPoly (x1, x2, y1, y2)))
	return 0;
    }
  else
    {
      if (!
	  (np = LinePoly ((LineType *) pad, pad->Thickness + pad->Clearance)))
	return 0;
    }
  return Subtract (np, p, True);
}

struct cpInfo
{
  const BoxType *other;
  DataType *data;
  LayerType *layer;
  PolygonType *polygon;
  Boolean solder;
};

static int
pin_sub_callback (const BoxType * b, void *cl)
{
  PinTypePtr pin = (PinTypePtr) b;
  struct cpInfo *info = (struct cpInfo *) cl;
  PolygonTypePtr polygon;

  /* don't subtract the object that was put back! */
  if (b == info->other)
    return 0;
  polygon = info->polygon;
  return SubtractPin (info->data, pin, info->layer, polygon);
}

static int
arc_sub_callback (const BoxType * b, void *cl)
{
  ArcTypePtr arc = (ArcTypePtr) b;
  struct cpInfo *info = (struct cpInfo *) cl;
  PolygonTypePtr polygon;

  /* don't subtract the object that was put back! */
  if (b == info->other)
    return 0;
  if (!TEST_FLAG (CLEARLINEFLAG, arc))
    return 0;
  polygon = info->polygon;
  return SubtractArc (arc, polygon);
}

static int
pad_sub_callback (const BoxType * b, void *cl)
{
  PadTypePtr pad = (PadTypePtr) b;
  struct cpInfo *info = (struct cpInfo *) cl;
  PolygonTypePtr polygon;

  /* don't subtract the object that was put back! */
  if (b == info->other)
    return 0;
  polygon = info->polygon;
  if (XOR (TEST_FLAG (ONSOLDERFLAG, pad), !info->solder))
    return SubtractPad (pad, polygon);
  return 0;
}

static int
line_sub_callback (const BoxType * b, void *cl)
{
  LineTypePtr line = (LineTypePtr) b;
  struct cpInfo *info = (struct cpInfo *) cl;
  PolygonTypePtr polygon;

  /* don't subtract the object that was put back! */
  if (b == info->other)
    return 0;
  if (!TEST_FLAG (CLEARLINEFLAG, line))
    return 0;
  polygon = info->polygon;
  return SubtractLine (line, polygon);
}

static int Group (DataTypePtr Data, Cardinal layer)
{
  Cardinal i, j;
  for (i =0; i < max_layer; i++)
    for (j = 0; j < ((PCBType *)(Data->pcb))->LayerGroups.Number[i]; j++)
      if (layer == ((PCBType *)(Data->pcb))->LayerGroups.Entries[i][j])
        return i;
  return i;
}

static int
clearPoly (DataTypePtr Data, LayerTypePtr Layer, PolygonType * polygon,
	   const BoxType * here, BDimension expand)
{
  int r = 0;
  BoxType region;
  struct cpInfo info;
  Cardinal group;

  if (!TEST_FLAG (CLEARPOLYFLAG, polygon))
    return 0;
  group = Group (Data, GetLayerNumber (Data, Layer));
  info.solder = (group == Group (Data, max_layer + SOLDER_LAYER));
  info.data = Data;
  info.other = here;
  info.layer = Layer;
  info.polygon = polygon;
  if (here)
    region = clip_box (here, &polygon->BoundingBox);
  else
    region = polygon->BoundingBox;
  shrink_box (&region, -expand);

  r = r_search (Data->via_tree, &region, NULL, pin_sub_callback, &info);
  r += r_search (Data->pin_tree, &region, NULL, pin_sub_callback, &info);
  GROUP_LOOP (Data, group);
  {
    r += r_search (layer->line_tree, &region, NULL, line_sub_callback, &info);
    r += r_search (layer->arc_tree, &region, NULL, arc_sub_callback, &info);
    if (info.solder || group == GetLayerGroupNumberByNumber (max_layer + COMPONENT_LAYER))
      r += r_search (Data->pad_tree, &region, NULL, pad_sub_callback, &info);
  }
  END_LOOP;
  return r;
}

static int
Unsubtract (POLYAREA * np1, PolygonType * p)
{
  POLYAREA *merged = NULL, *np = np1;;
  int x;
  assert (np);
  assert (p && p->Clipped);
  x = poly_Boolean_free (p->Clipped, np, &merged, PBO_UNITE);
  if (x != err_ok)
    {
      fprintf (stderr, "Error while clipping PBO_UNITE\n");
      poly_Free (&merged);
      p->Clipped = NULL;
      return 0;
    }
  p->Clipped = biggest (merged);
  assert (!p->Clipped || poly_Valid (p->Clipped));
  return ClipOriginal (p);
}

static int
UnsubtractPin (PinType * pin, LayerType * l, PolygonType * p)
{
  POLYAREA *np;

  /* overlap a bit to prevent gaps from rounding errors */
  np = PinPoly (pin, (pin->Thickness + pin->Clearance) * 1.1);

  if (!np)
    return 0;
  if (!Unsubtract (np, p))
    return 0;
  clearPoly (PCB->Data, l, p, (const BoxType *) pin, 0.1 * (pin->Thickness + pin->Clearance));
  return 1;
}

static int
UnsubtractArc (ArcType * arc, LayerType * l, PolygonType * p)
{
  POLYAREA *np = NULL;

  if (!TEST_FLAG (CLEARLINEFLAG, arc))
    return 0;
  /* overlap a bit to prevent notches from rounding errors */
  if (!(np = ArcPoly (arc, arc->Thickness + arc->Clearance + 100)))
    return 0;
  if (!Unsubtract (np, p))
    return 0;
  clearPoly (PCB->Data, l, p, (const BoxType *) arc, 50);
  return 1;
}

static int
UnsubtractLine (LineType * line, LayerType * l, PolygonType * p)
{
  POLYAREA *np = NULL;

  if (!TEST_FLAG (CLEARLINEFLAG, line))
    return 0;
  /* overlap a bit to prevent notches from rounding errors */
  if (!(np = LinePoly (line, line->Thickness + line->Clearance + 100)))
    return 0;
  if (!Unsubtract (np, p))
    return 0;
  clearPoly (PCB->Data, l, p, (const BoxType *) line, 50);
  return 1;
}

static int
UnsubtractPad (PadType * pad, LayerType * l, PolygonType * p)
{
  POLYAREA *np = NULL;

  if (TEST_FLAG (SQUAREFLAG, pad))
    {
      BDimension t = ((pad->Thickness + pad->Clearance) / 2) + 100;
      LocationType x1, x2, y1, y2;
      x1 = MIN (pad->Point1.X, pad->Point2.X) - t;
      x2 = MAX (pad->Point1.X, pad->Point2.X) + t;
      y1 = MIN (pad->Point1.Y, pad->Point2.Y) - t;
      y2 = MAX (pad->Point1.Y, pad->Point2.Y) + t;
      if (!(np = RectPoly (x1, x2, y1, y2)))
	return 0;
    }
  else
    {
      /* overlap a bit to prevent notches from rounding errors */
      if (!
	  (np =
	   LinePoly ((LineType *) pad,
		     pad->Thickness + pad->Clearance + 100)))
	return 0;
    }
  if (!Unsubtract (np, p))
    return 0;
  clearPoly (PCB->Data, l, p, (const BoxType *) pad, 50);
  return 1;
}

int
InitClip (DataTypePtr Data, LayerTypePtr layer, PolygonType * p)
{
  if (p->Clipped)
    poly_Free (&p->Clipped);
  p->Clipped = original_poly (p);
  if (!p->Clipped)
    return 0;
  assert (poly_Valid (p->Clipped));
  if (TEST_FLAG (CLEARPOLYFLAG, p))
    clearPoly (Data, layer, p, NULL, 0);
  return 1;
}

/* --------------------------------------------------------------------------
 * remove redundant polygon points. Any point that lies on the straight
 * line between the points on either side of it is redundant.
 * returns true if any points are removed
 */
Boolean
RemoveExcessPolygonPoints (LayerTypePtr Layer, PolygonTypePtr Polygon)
{
  PointTypePtr pt1, pt2, pt3;
  Cardinal n;
  LineType line;
  Boolean changed = False;

  if (Undoing ())
    return (False);
  /* there are always at least three points in a polygon */
  pt1 = &Polygon->Points[Polygon->PointN - 1];
  pt2 = &Polygon->Points[0];
  pt3 = &Polygon->Points[1];
  for (n = 0; n < Polygon->PointN; n++, pt1++, pt2++, pt3++)
    {
      /* wrap around polygon */
      if (n == 1)
	pt1 = &Polygon->Points[0];
      if (n == Polygon->PointN - 1)
	pt3 = &Polygon->Points[0];
      line.Point1 = *pt1;
      line.Point2 = *pt3;
      line.Thickness = 0;
      if (IsPointOnLine ((float) pt2->X, (float) pt2->Y, 0.0, &line))
	{
	  RemoveObject (POLYGONPOINT_TYPE, (void *) Layer, (void *) Polygon,
			(void *) pt2);
	  changed = True;
	}
    }
  return (changed);
}

/* ---------------------------------------------------------------------------
 * returns the index of the polygon point which is the end
 * point of the segment with the lowest distance to the passed
 * coordinates
 */
Cardinal
GetLowestDistancePolygonPoint (PolygonTypePtr Polygon, LocationType X,
			       LocationType Y)
{
  double mindistance = (double) MAX_COORD * MAX_COORD;
  PointTypePtr ptr1 = &Polygon->Points[Polygon->PointN - 1],
    ptr2 = &Polygon->Points[0];
  Cardinal n, result = 0;

  /* we calculate the distance to each segment and choose the
   * shortest distance. If the closest approach between the
   * given point and the projected line (i.e. the segment extended)
   * is not on the segment, then the distance is the distance
   * to the segment end point.
   */

  for (n = 0; n < Polygon->PointN; n++, ptr2++)
    {
      register double u, dx, dy;
      dx = ptr2->X - ptr1->X;
      dy = ptr2->Y - ptr1->Y;
      if (dx != 0.0 || dy != 0.0)
	{
	  /* projected intersection is at P1 + u(P2 - P1) */
	  u = ((X - ptr1->X) * dx + (Y - ptr1->Y) * dy) / (dx * dx + dy * dy);

	  if (u < 0.0)
	    {			/* ptr1 is closest point */
	      u = SQUARE (X - ptr1->X) + SQUARE (Y - ptr1->Y);
	    }
	  else if (u > 1.0)
	    {			/* ptr2 is closest point */
	      u = SQUARE (X - ptr2->X) + SQUARE (Y - ptr2->Y);
	    }
	  else
	    {			/* projected intersection is closest point */
	      u = SQUARE (X - ptr1->X * (1.0 - u) - u * ptr2->X) +
		SQUARE (Y - ptr1->Y * (1.0 - u) - u * ptr2->Y);
	    }
	  if (u < mindistance)
	    {
	      mindistance = u;
	      result = n;
	    }
	}
      ptr1 = ptr2;
    }
  return (result);
}

/* ---------------------------------------------------------------------------
 * go back to the  previous point of the polygon
 */
void
GoToPreviousPoint (void)
{
  switch (Crosshair.AttachedPolygon.PointN)
    {
      /* do nothing if mode has just been entered */
    case 0:
      break;

      /* reset number of points and 'LINE_MODE' state */
    case 1:
      Crosshair.AttachedPolygon.PointN = 0;
      Crosshair.AttachedLine.State = STATE_FIRST;
      addedLines = 0;
      break;

      /* back-up one point */
    default:
      {
	PointTypePtr points = Crosshair.AttachedPolygon.Points;
	Cardinal n = Crosshair.AttachedPolygon.PointN - 2;

	Crosshair.AttachedPolygon.PointN--;
	Crosshair.AttachedLine.Point1.X = points[n].X;
	Crosshair.AttachedLine.Point1.Y = points[n].Y;
	break;
      }
    }
}

/* ---------------------------------------------------------------------------
 * close polygon if possible
 */
void
ClosePolygon (void)
{
  Cardinal n = Crosshair.AttachedPolygon.PointN;

  /* check number of points */
  if (n >= 3)
    {
      /* if 45 degree lines are what we want do a quick check
       * if closing the polygon makes sense
       */
      if (!TEST_FLAG (ALLDIRECTIONFLAG, PCB))
	{
	  BDimension dx, dy;

	  dx = abs (Crosshair.AttachedPolygon.Points[n - 1].X -
		    Crosshair.AttachedPolygon.Points[0].X);
	  dy = abs (Crosshair.AttachedPolygon.Points[n - 1].Y -
		    Crosshair.AttachedPolygon.Points[0].Y);
	  if (!(dx == 0 || dy == 0 || dx == dy))
	    {
	      Message
		(_
		 ("Cannot close polygon because 45 degree lines are requested.\n"));
	      return;
	    }
	}
      CopyAttachedPolygonToLayer ();
      Draw ();
    }
  else
    Message (_("A polygon has to have at least 3 points\n"));
}

/* ---------------------------------------------------------------------------
 * moves the data of the attached (new) polygon to the current layer
 */
void
CopyAttachedPolygonToLayer (void)
{
  PolygonTypePtr polygon;
  int saveID;

  /* move data to layer and clear attached struct */
  polygon = CreateNewPolygon (CURRENT, NoFlags ());
  saveID = polygon->ID;
  *polygon = Crosshair.AttachedPolygon;
  polygon->ID = saveID;
  SET_FLAG (CLEARPOLYFLAG, polygon);
  memset (&Crosshair.AttachedPolygon, 0, sizeof (PolygonType));
  SetPolygonBoundingBox (polygon);
  if (!CURRENT->polygon_tree)
    CURRENT->polygon_tree = r_create_tree (NULL, 0, 0);
  r_insert_entry (CURRENT->polygon_tree, (BoxType *) polygon, 0);
  InitClip (PCB->Data, CURRENT, polygon);
  DrawPolygon (CURRENT, polygon, 0);
  SetChangedFlag (True);

  /* reset state of attached line */
  Crosshair.AttachedLine.State = STATE_FIRST;
  addedLines = 0;

  /* add to undo list */
  AddObjectToCreateUndoList (POLYGON_TYPE, CURRENT, polygon, polygon);
  IncrementUndoSerialNumber ();
}

struct hole_info
{
  LayerTypePtr layer;
  const BoxType *range;
  int (*callback) (PLINE *, LayerTypePtr, PolygonTypePtr);
  jmp_buf env, env0;
};

static int
hole_callback (const BoxType * b, void *cl)
{
  struct hole_info *hole = (struct hole_info *) cl;
  PolygonTypePtr polygon = (PolygonTypePtr) b;
  POLYAREA *pa;
  PLINE *pl;

  pa = polygon->Clipped;
  /* If this hole is so big the polygon doesn't exist, then it's not
   * really a hole.
   */
  if (!pa)
    return 0;
  do
    {
      for (pl = pa->contours->next; pl; pl = pl->next)
	{
	  if (pl->xmin > hole->range->X2 || pl->xmax < hole->range->X1 ||
	      pl->ymin > hole->range->Y2 || pl->ymax < hole->range->Y1)
	    continue;
	  if (hole->callback (pl, hole->layer, polygon))
	    {
	      longjmp (hole->env, 1);
	    }
	}
    }
  while ((pa = pa->f) != polygon->Clipped);
  return 0;
}

/* find polygon holes in range, then call the callback function for
 * each hole. If the callback returns non-zero, stop
 * the search.
 */
int
PolygonHoles (int group, const BoxType * range,
	      int (*any_call) (PLINE * contour,
			       LayerTypePtr lay, PolygonTypePtr poly))
{
  struct hole_info info;

  info.callback = any_call;
  info.range = range;
  GROUP_LOOP (PCB->Data, group);
  {
    if (!layer->PolygonN)
      continue;
    info.layer = layer;
    if (setjmp (info.env0) == 0)
      r_search (layer->polygon_tree, range, NULL, hole_callback, &info);
    else
      return 1;
  }
  END_LOOP;
  return 0;
}

struct plow_info
{
  int type;
  void *ptr1, *ptr2;
  LayerTypePtr layer;
  DataTypePtr data;
  int (*callback) (DataTypePtr, LayerTypePtr, PolygonTypePtr, int, void *,
		   void *);
};

static int
subtract_plow (DataTypePtr Data, LayerTypePtr Layer, PolygonTypePtr Polygon,
	       int type, void *ptr1, void *ptr2)
{
  switch (type)
    {
    case PIN_TYPE:
    case VIA_TYPE:
      SubtractPin (Data, (PinTypePtr) ptr2, Layer, Polygon);
      return 1;
    case LINE_TYPE:
      SubtractLine ((LineTypePtr) ptr2, Polygon);
      return 1;
    case ARC_TYPE:
      SubtractArc ((ArcTypePtr) ptr2, Polygon);
      return 1;
    case PAD_TYPE:
      SubtractPad ((PadTypePtr) ptr2, Polygon);
      return 1;
    }
  return 0;
}

static int
add_plow (DataTypePtr Data, LayerTypePtr Layer, PolygonTypePtr Polygon,
	  int type, void *ptr1, void *ptr2)
{
  switch (type)
    {
    case PIN_TYPE:
    case VIA_TYPE:
      UnsubtractPin ((PinTypePtr) ptr2, Layer, Polygon);
      return 1;
    case LINE_TYPE:
      UnsubtractLine ((LineTypePtr) ptr2, Layer, Polygon);
      return 1;
    case ARC_TYPE:
      UnsubtractArc ((ArcTypePtr) ptr2, Layer, Polygon);
      return 1;
    case PAD_TYPE:
      UnsubtractPad ((PadTypePtr) ptr2, Layer, Polygon);
      return 1;
    }
  return 0;
}

static int
plow_callback (const BoxType * b, void *cl)
{
  struct plow_info *plow = (struct plow_info *) cl;
  PolygonTypePtr polygon = (PolygonTypePtr) b;

  return plow->callback (plow->data, plow->layer, polygon, plow->type,
			 plow->ptr1, plow->ptr2);
}

int
PlowsPolygon (DataType * Data, int type, void *ptr1, void *ptr2,
	      int (*call_back) (DataTypePtr data, LayerTypePtr lay,
				PolygonTypePtr poly, int type, void *ptr1,
				void *ptr2))
{
  BoxType sb = ((PinTypePtr) ptr2)->BoundingBox;
  int r = 0;
  struct plow_info info;

  info.type = type;
  info.ptr1 = ptr1;
  info.ptr2 = ptr2;
  info.data = Data;
  info.callback = call_back;
  switch (type)
    {
    case PIN_TYPE:
    case VIA_TYPE:
      if (type == PIN_TYPE || ptr1 == ptr2 || ptr1 == NULL)
	{
	  LAYER_LOOP (Data, max_layer);
	  {
	    info.layer = layer;
	    r +=
	      r_search (layer->polygon_tree, &sb, NULL, plow_callback, &info);
	  }
	  END_LOOP;
	}
      else
	{
	  GROUP_LOOP (Data, GetLayerGroupNumberByNumber (GetLayerNumber (Data,
									 ((LayerTypePtr) ptr1))));
	  {
	    info.layer = layer;
	    r +=
	      r_search (layer->polygon_tree, &sb, NULL, plow_callback, &info);
	  }
	  END_LOOP;
	}
      break;
    case LINE_TYPE:
    case ARC_TYPE:
      /* the cast works equally well for lines and arcs */
      if (!TEST_FLAG (CLEARLINEFLAG, (LineTypePtr) ptr2))
	return 0;
      GROUP_LOOP (Data, GetLayerGroupNumberByNumber (GetLayerNumber (Data,
								     ((LayerTypePtr) ptr1))));
      {
	info.layer = layer;
	r += r_search (layer->polygon_tree, &sb, NULL, plow_callback, &info);
      }
      END_LOOP;
      break;
    case PAD_TYPE:
      {
	Cardinal group = TEST_FLAG (ONSOLDERFLAG,
				    (PadType *) ptr2) ? SOLDER_LAYER :
	  COMPONENT_LAYER;
	group = GetLayerGroupNumberByNumber (max_layer + group);
	GROUP_LOOP (Data, group);
	{
	  info.layer = layer;
	  r +=
	    r_search (layer->polygon_tree, &sb, NULL, plow_callback, &info);
	}
	END_LOOP;
      }
      break;

    case ELEMENT_TYPE:
      {
	PIN_LOOP ((ElementType *) ptr1);
	{
	  PlowsPolygon (Data, PIN_TYPE, ptr1, pin, call_back);
	}
	END_LOOP;
	PAD_LOOP ((ElementType *) ptr1);
	{
	  PlowsPolygon (Data, PAD_TYPE, ptr1, pad, call_back);
	}
	END_LOOP;
      }
      break;
    }
  return r;
}

void
RestoreToPolygon (DataType * Data, int type, void *ptr1, void *ptr2)
{
  PlowsPolygon (Data, type, ptr1, ptr2, add_plow);
}

void
ClearFromPolygon (DataType * Data, int type, void *ptr1, void *ptr2)
{
  if (type != POLYGON_TYPE)
    // InitClip (PCB->Data, (LayerTypePtr) ptr1, (PolygonTypePtr) ptr2);
    //else
    PlowsPolygon (Data, type, ptr1, ptr2, subtract_plow);
}

Boolean
isects (POLYAREA * a, PolygonTypePtr p, Boolean fr)
{
  POLYAREA *x;
  Boolean ans;
  ans = Touching (a, p->Clipped);
  /* argument may be register, so we must copy it */
  x = a;
  if (fr)
    poly_Free (&x);
  return ans;
}


Boolean
IsPointInPolygon (LocationType X, LocationType Y, BDimension r,
		  PolygonTypePtr p)
{
  POLYAREA *c;
  Vector v;
  v[0] = X;
  v[1] = Y;
  if (poly_CheckInside( p->Clipped, v))
    return True;
  r = max (r, 1);
  if (!(c = CirclePoly (X, Y, r)))
    return False;
  return isects (c, p, True);
}

Boolean
IsRectangleInPolygon (LocationType X1, LocationType Y1, LocationType X2,
		      LocationType Y2, PolygonTypePtr p)
{
  POLYAREA *s;
  if (!
      (s = RectPoly (min (X1, X2), max (X1, X2), min (Y1, Y2), max (Y1, Y2))))
    return False;
  return isects (s, p, True);
}

void
NoHolesPolygonDicer (PLINE * p, void (*emit) (PolygonTypePtr))
{
  POLYAREA pa;

  pa.f = pa.b = &pa;
  pa.contours = p;
  if (!p->next)			/* no holes */
    {
      PolygonType poly;
      PointType pts[4];

      poly.BoundingBox.X1 = p->xmin;
      poly.BoundingBox.X2 = p->xmax;
      poly.BoundingBox.Y1 = p->ymin;
      poly.BoundingBox.Y2 = p->ymax;
      poly.PointN = poly.PointMax = 4;
      poly.Clipped = &pa;
      poly.Points = pts;
      pts[0].X = pts[0].X2 = p->xmin;
      pts[0].Y = pts[0].Y2 = p->ymin;
      pts[1].X = pts[1].X2 = p->xmax;
      pts[1].Y = pts[1].Y2 = p->ymin;
      pts[2].X = pts[2].X2 = p->xmax;
      pts[2].Y = pts[2].Y2 = p->ymax;
      pts[3].X = pts[3].X2 = p->xmin;
      pts[3].Y = pts[3].Y2 = p->ymax;
      poly.Flags = MakeFlags (CLEARPOLYFLAG);
      emit (&poly);
      return;
    }
  else
    {
      POLYAREA *poly2, *res = NULL;

      /* make a rectangle of the left region slicing through the middle of the first hole */
      poly2 = RectPoly (p->xmin, (p->next->xmin + p->next->xmax) / 2, p->ymin, p->ymax);
      poly_Boolean (poly2, &pa, &res, PBO_ISECT);
      poly_Free (&poly2);
      if (res)
	{
	  POLYAREA *x;
	  x = res;
	  do
	    {
	      PLINE *pl = x->contours;
	      NoHolesPolygonDicer (pl, emit);
	    }
	  while ((x = x->f) != res);
	  poly_Free (&res);
	}
      /* make a rectangle of the right region slicing through the middle of the first hole */
      poly2 = RectPoly ((p->next->xmin + p->next->xmax) / 2, p->xmax, p->ymin, p->ymax);
      poly_Boolean (poly2, &pa, &res, PBO_ISECT);
      poly_Free (&poly2);
      if (res)
	{
	  POLYAREA *x;
	  x = res;
	  do
	    {
	      PLINE *pl = x->contours;
	      NoHolesPolygonDicer (pl, emit);
	    }
	  while ((x = x->f) != res);
	  poly_Free (&res);
	}
    }
}


/* make a polygon split into multiple parts into multiple polygons */
Boolean
MorphPolygon (LayerTypePtr layer, PolygonTypePtr poly)
{
  POLYAREA *p, *start;
  Boolean many = False;
  FlagType flags;

  if (!poly->Clipped || TEST_FLAG (LOCKFLAG, poly))
    return False;
  if (poly->Clipped->f == poly->Clipped)
    return False;
  ErasePolygon (poly);
  start = p = poly->Clipped;
  /* This is ugly. The creation of the new polygons can cause
   * all of the polygon pointers (including the one we're called
   * with to change if there is a realloc in GetPolygonMemory().
   * That will invalidate our original "poly" argument, potentially
   * inside the loop. We need to steal the Clipped pointer and
   * hide it from the Remove call so that it still exists while
   * we do this dirty work.
   */
  poly->Clipped = NULL;
  flags = poly->Flags;
  RemovePolygon (layer, poly);
  do
    {
      VNODE *v;
      PolygonTypePtr new;

      if (p->contours->area > M_PI * PCB->Bloat * 0.5 * PCB->Bloat)
	{
	  new = CreateNewPolygon (layer, flags);
	  if (!new)
	    return False;
	  many = True;
	  v = &p->contours->head;
	  CreateNewPointInPolygon (new, v->point[0], v->point[1]);
	  for (v = v->next; v != &p->contours->head; v = v->next)
	    CreateNewPointInPolygon (new, v->point[0], v->point[1]);
	  SetPolygonBoundingBox (new);
	  AddObjectToCreateUndoList (POLYGON_TYPE, layer, new, new);
	  new->Clipped = p;
	  p = p->f;		/* go to next pline */
	  new->Clipped->b = new->Clipped->f = new->Clipped;	/* unlink from others */
	  r_insert_entry (layer->polygon_tree, (BoxType *) new, 0);
	  DrawPolygon (layer, new, 0);
	}
      else
	{
	  POLYAREA *t = p;

	  p = p->f;
	  poly_DelContour (&t->contours);
	  free (t);
	}
    }
  while (p != start);
  return many;
}
