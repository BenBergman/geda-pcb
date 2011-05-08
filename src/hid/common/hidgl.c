/*
 *                            COPYRIGHT
 *
 *  PCB, interactive printed circuit board design
 *  Copyright (C) 2009-2011 PCB Contributers (See ChangeLog for details)
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <math.h>
#include <assert.h>

/* The Linux OpenGL ABI 1.0 spec requires that we define
 * GL_GLEXT_PROTOTYPES before including gl.h or glx.h for extensions
 * in order to get prototypes:
 *   http://www.opengl.org/registry/ABI/
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glu.h>

#include "action.h"
#include "crosshair.h"
#include "data.h"
#include "draw.h"
#include "error.h"
#include "global.h"
#include "mymem.h"
#include "draw.h"
#include "clip.h"

#include "hid.h"
#include "hidgl.h"
#include "rtree.h"

#ifdef HAVE_LIBDMALLOC
#include <dmalloc.h>
#endif


triangle_buffer buffer;
float global_depth = 0;

void
hidgl_init_triangle_array (triangle_buffer *buffer)
{
  glEnableClientState (GL_VERTEX_ARRAY);
  glVertexPointer (3, GL_FLOAT, 0, buffer->triangle_array);
  buffer->triangle_count = 0;
  buffer->coord_comp_count = 0;
}

void
hidgl_flush_triangles (triangle_buffer *buffer)
{
  if (buffer->triangle_count == 0)
    return;

  glDrawArrays (GL_TRIANGLES, 0, buffer->triangle_count * 3);
  buffer->triangle_count = 0;
  buffer->coord_comp_count = 0;
}

void
hidgl_ensure_triangle_space (triangle_buffer *buffer, int count)
{
  if (count > TRIANGLE_ARRAY_SIZE)
    {
      fprintf (stderr, "Not enough space in vertex buffer\n");
      fprintf (stderr, "Requested %i triangles, %i available\n",
                       count, TRIANGLE_ARRAY_SIZE);
      exit (1);
    }
  if (count > TRIANGLE_ARRAY_SIZE - buffer->triangle_count)
    hidgl_flush_triangles (buffer);
}

void
hidgl_set_depth (float depth)
{
  global_depth = depth;
}

void
hidgl_draw_grid (BoxType *drawn_area)
{
  static GLfloat *points = 0;
  static int npoints = 0;
  int x1, y1, x2, y2, n, i;
  double x, y;

  if (!Settings.DrawGrid)
    return;

  x1 = GRIDFIT_X (MAX (0, drawn_area->X1), PCB->Grid);
  y1 = GRIDFIT_Y (MAX (0, drawn_area->Y1), PCB->Grid);
  x2 = GRIDFIT_X (MIN (PCB->MaxWidth, drawn_area->X2), PCB->Grid);
  y2 = GRIDFIT_Y (MIN (PCB->MaxHeight, drawn_area->Y2), PCB->Grid);

  if (x1 > x2)
    {
      int tmp = x1;
      x1 = x2;
      x2 = tmp;
    }

  if (y1 > y2)
    {
      int tmp = y1;
      y1 = y2;
      y2 = tmp;
    }

  n = (int) ((x2 - x1) / PCB->Grid + 0.5) + 1;
  if (n > npoints)
    {
      npoints = n + 10;
      points = realloc (points, npoints * 3 * sizeof (GLfloat));
    }

  glEnableClientState (GL_VERTEX_ARRAY);
  glVertexPointer (3, GL_FLOAT, 0, points);

  n = 0;
  for (x = x1; x <= x2; x += PCB->Grid)
    {
      points[3 * n + 0] = x;
      points[3 * n + 2] = global_depth;
      n++;
    }
  for (y = y1; y <= y2; y += PCB->Grid)
    {
      for (i = 0; i < n; i++)
        points[3 * i + 1] = y;
      glDrawArrays (GL_POINTS, 0, n);
    }

  glDisableClientState (GL_VERTEX_ARRAY);
}

#define MAX_PIXELS_ARC_TO_CHORD 0.5
#define MIN_SLICES 6
int calc_slices (float pix_radius, float sweep_angle)
{
  float slices;

  if (pix_radius <= MAX_PIXELS_ARC_TO_CHORD)
    return MIN_SLICES;

  slices = sweep_angle / acosf (1 - MAX_PIXELS_ARC_TO_CHORD / pix_radius) / 2.;
  return (int)ceilf (slices);
}

#define MIN_TRIANGLES_PER_CAP 3
#define MAX_TRIANGLES_PER_CAP 90
static void draw_cap (double width, int x, int y, double angle, double scale)
{
  float last_capx, last_capy;
  float capx, capy;
  float radius = width / 2.;
  int slices = calc_slices (radius / scale, M_PI);
  int i;

  if (slices < MIN_TRIANGLES_PER_CAP)
    slices = MIN_TRIANGLES_PER_CAP;

  if (slices > MAX_TRIANGLES_PER_CAP)
    slices = MAX_TRIANGLES_PER_CAP;

  hidgl_ensure_triangle_space (&buffer, slices);

  last_capx =  radius * cosf (angle * M_PI / 180.) + x;
  last_capy = -radius * sinf (angle * M_PI / 180.) + y;
  for (i = 0; i < slices; i++) {
    capx =  radius * cosf (angle * M_PI / 180. + ((float)(i + 1)) * M_PI / (float)slices) + x;
    capy = -radius * sinf (angle * M_PI / 180. + ((float)(i + 1)) * M_PI / (float)slices) + y;
    hidgl_add_triangle (&buffer, last_capx, last_capy, capx, capy, x, y);
    last_capx = capx;
    last_capy = capy;
  }
}

void
hidgl_draw_line (int cap, double width, int x1, int y1, int x2, int y2, double scale)
{
  double angle;
  float deltax, deltay, length;
  float wdx, wdy;
  int circular_caps = 0;
  int hairline = 0;

  if (width == 0.0)
    hairline = 1;

  if (width < scale)
    width = scale;

  deltax = x2 - x1;
  deltay = y2 - y1;

  length = sqrt (deltax * deltax + deltay * deltay);

  if (length == 0) {
    /* Assume the orientation of the line is horizontal */
    angle = 0;
    wdx = -width / 2.;
    wdy = 0;
    length = 1.;
    deltax = 1.;
    deltay = 0.;
  } else {
    wdy = deltax * width / 2. / length;
    wdx = -deltay * width / 2. / length;

    if (deltay == 0.)
      angle = (deltax < 0) ? 270. : 90.;
    else
      angle = 180. / M_PI * atanl (deltax / deltay);

    if (deltay < 0)
      angle += 180.;
  }

  switch (cap) {
    case Trace_Cap:
    case Round_Cap:
      circular_caps = 1;
      break;

    case Square_Cap:
    case Beveled_Cap:
      x1 -= deltax * width / 2. / length;
      y1 -= deltay * width / 2. / length;
      x2 += deltax * width / 2. / length;
      y2 += deltay * width / 2. / length;
      break;
  }

  hidgl_ensure_triangle_space (&buffer, 2);
  hidgl_add_triangle (&buffer, x1 - wdx, y1 - wdy,
                               x2 - wdx, y2 - wdy,
                               x2 + wdx, y2 + wdy);
  hidgl_add_triangle (&buffer, x1 - wdx, y1 - wdy,
                               x2 + wdx, y2 + wdy,
                               x1 + wdx, y1 + wdy);

  /* Don't bother capping hairlines */
  if (circular_caps && !hairline)
    {
      draw_cap (width, x1, y1, angle, scale);
      draw_cap (width, x2, y2, angle + 180., scale);
    }
}

#define MIN_SLICES_PER_ARC 6
#define MAX_SLICES_PER_ARC 360
void
hidgl_draw_arc (double width, int x, int y, int rx, int ry,
                int start_angle, int delta_angle, double scale)
{
  float last_inner_x, last_inner_y;
  float last_outer_x, last_outer_y;
  float inner_x, inner_y;
  float outer_x, outer_y;
  float inner_r;
  float outer_r;
  float cos_ang, sin_ang;
  float start_angle_rad;
  float delta_angle_rad;
  float angle_incr_rad;
  int slices;
  int i;
  int hairline = 0;

  if (width == 0.0)
    hairline = 1;

  if (width < scale)
    width = scale;

  inner_r = rx - width / 2.;
  outer_r = rx + width / 2.;

  if (delta_angle < 0) {
    start_angle += delta_angle;
    delta_angle = - delta_angle;
  }

  start_angle_rad = start_angle * M_PI / 180.;
  delta_angle_rad = delta_angle * M_PI / 180.;

  slices = calc_slices ((rx + width / 2.) / scale, delta_angle_rad);

  if (slices < MIN_SLICES_PER_ARC)
    slices = MIN_SLICES_PER_ARC;

  if (slices > MAX_SLICES_PER_ARC)
    slices = MAX_SLICES_PER_ARC;

  hidgl_ensure_triangle_space (&buffer, 2 * slices);

  angle_incr_rad = delta_angle_rad / (float)slices;

  cos_ang = cosf (start_angle_rad);
  sin_ang = sinf (start_angle_rad);
  last_inner_x = -inner_r * cos_ang + x;  last_inner_y = inner_r * sin_ang + y;
  last_outer_x = -outer_r * cos_ang + x;  last_outer_y = outer_r * sin_ang + y;
  for (i = 1; i <= slices; i++) {
    cos_ang = cosf (start_angle_rad + ((float)(i)) * angle_incr_rad);
    sin_ang = sinf (start_angle_rad + ((float)(i)) * angle_incr_rad);
    inner_x = -inner_r * cos_ang + x;  inner_y = inner_r * sin_ang + y;
    outer_x = -outer_r * cos_ang + x;  outer_y = outer_r * sin_ang + y;
    hidgl_add_triangle (&buffer, last_inner_x, last_inner_y,
                                 last_outer_x, last_outer_y,
                                 outer_x, outer_y);
    hidgl_add_triangle (&buffer, last_inner_x, last_inner_y,
                                 inner_x, inner_y,
                                 outer_x, outer_y);
    last_inner_x = inner_x;  last_inner_y = inner_y;
    last_outer_x = outer_x;  last_outer_y = outer_y;
  }

  /* Don't bother capping hairlines */
  if (hairline)
    return;

  draw_cap (width, x + rx * -cosf (start_angle_rad),
                   y + rx *  sinf (start_angle_rad),
                   start_angle, scale);
  draw_cap (width, x + rx * -cosf (start_angle_rad + delta_angle_rad),
                   y + rx *  sinf (start_angle_rad + delta_angle_rad),
                   start_angle + delta_angle + 180., scale);
}

void
hidgl_draw_rect (int x1, int y1, int x2, int y2)
{
  glBegin (GL_LINE_LOOP);
  glVertex3f (x1, y1, global_depth);
  glVertex3f (x1, y2, global_depth);
  glVertex3f (x2, y2, global_depth);
  glVertex3f (x2, y1, global_depth);
  glEnd ();
}


void
hidgl_fill_circle (int vx, int vy, int vr, double scale)
{
#define MIN_TRIANGLES_PER_CIRCLE 6
#define MAX_TRIANGLES_PER_CIRCLE 360
  float last_x, last_y;
  float radius = vr;
  int slices;
  int i;

  slices = calc_slices (vr / scale, 2 * M_PI);

  if (slices < MIN_TRIANGLES_PER_CIRCLE)
    slices = MIN_TRIANGLES_PER_CIRCLE;

  if (slices > MAX_TRIANGLES_PER_CIRCLE)
    slices = MAX_TRIANGLES_PER_CIRCLE;

  hidgl_ensure_triangle_space (&buffer, slices);

  last_x = vx + vr;
  last_y = vy;

  for (i = 0; i < slices; i++) {
    float x, y;
    x = radius * cosf (((float)(i + 1)) * 2. * M_PI / (float)slices) + vx;
    y = radius * sinf (((float)(i + 1)) * 2. * M_PI / (float)slices) + vy;
    hidgl_add_triangle (&buffer, vx, vy, last_x, last_y, x, y);
    last_x = x;
    last_y = y;
  }
}

#define MAX_COMBINED_MALLOCS 2500
static void *combined_to_free [MAX_COMBINED_MALLOCS];
static int combined_num_to_free = 0;

static GLenum tessVertexType;
static int stashed_vertices;
static int triangle_comp_idx;


static void
myError (GLenum errno)
{
  printf ("gluTess error: %s\n", gluErrorString (errno));
}

static void
myFreeCombined ()
{
  while (combined_num_to_free)
    free (combined_to_free [-- combined_num_to_free]);
}

static void
myCombine ( GLdouble coords[3], void *vertex_data[4], GLfloat weight[4], void **dataOut )
{
#define MAX_COMBINED_VERTICES 2500
  static GLdouble combined_vertices [3 * MAX_COMBINED_VERTICES];
  static int num_combined_vertices = 0;

  GLdouble *new_vertex;

  if (num_combined_vertices < MAX_COMBINED_VERTICES)
    {
      new_vertex = &combined_vertices [3 * num_combined_vertices];
      num_combined_vertices ++;
    }
  else
    {
      new_vertex = malloc (3 * sizeof (GLdouble));

      if (combined_num_to_free < MAX_COMBINED_MALLOCS)
        combined_to_free [combined_num_to_free ++] = new_vertex;
      else
        printf ("myCombine leaking %lu bytes of memory\n", 3 * sizeof (GLdouble));
    }

  new_vertex[0] = coords[0];
  new_vertex[1] = coords[1];
  new_vertex[2] = coords[2];

  *dataOut = new_vertex;
}

static void
myBegin (GLenum type)
{
  tessVertexType = type;
  stashed_vertices = 0;
  triangle_comp_idx = 0;
}

static double global_scale;

static void
myVertex (GLdouble *vertex_data)
{
  static GLfloat triangle_vertices [2 * 3];

  if (tessVertexType == GL_TRIANGLE_STRIP ||
      tessVertexType == GL_TRIANGLE_FAN)
    {
      if (stashed_vertices < 2)
        {
          triangle_vertices [triangle_comp_idx ++] = vertex_data [0];
          triangle_vertices [triangle_comp_idx ++] = vertex_data [1];
          stashed_vertices ++;
        }
      else
        {
          hidgl_ensure_triangle_space (&buffer, 1);
          hidgl_add_triangle (&buffer,
                              triangle_vertices [0], triangle_vertices [1],
                              triangle_vertices [2], triangle_vertices [3],
                              vertex_data [0], vertex_data [1]);

          if (tessVertexType == GL_TRIANGLE_STRIP)
            {
              /* STRIP saves the last two vertices for re-use in the next triangle */
              triangle_vertices [0] = triangle_vertices [2];
              triangle_vertices [1] = triangle_vertices [3];
            }
          /* Both FAN and STRIP save the last vertex for re-use in the next triangle */
          triangle_vertices [2] = vertex_data [0];
          triangle_vertices [3] = vertex_data [1];
        }
    }
  else if (tessVertexType == GL_TRIANGLES)
    {
      triangle_vertices [triangle_comp_idx ++] = vertex_data [0];
      triangle_vertices [triangle_comp_idx ++] = vertex_data [1];
      stashed_vertices ++;
      if (stashed_vertices == 3)
        {
          hidgl_ensure_triangle_space (&buffer, 1);
          hidgl_add_triangle (&buffer,
                              triangle_vertices [0], triangle_vertices [1],
                              triangle_vertices [2], triangle_vertices [3],
                              triangle_vertices [4], triangle_vertices [5]);
          triangle_comp_idx = 0;
          stashed_vertices = 0;
        }
    }
  else
    printf ("Vertex recieved with unknown type\n");
}

void
hidgl_fill_polygon (int n_coords, int *x, int *y)
{
  int i;
  GLUtesselator *tobj;
  GLdouble *vertices;

  assert (n_coords > 0);

  vertices = malloc (sizeof(GLdouble) * n_coords * 3);

  tobj = gluNewTess ();
  gluTessCallback(tobj, GLU_TESS_BEGIN, myBegin);
  gluTessCallback(tobj, GLU_TESS_VERTEX, myVertex);
  gluTessCallback(tobj, GLU_TESS_COMBINE, myCombine);
  gluTessCallback(tobj, GLU_TESS_ERROR, myError);

  gluTessBeginPolygon (tobj, NULL);
  gluTessBeginContour (tobj);

  for (i = 0; i < n_coords; i++)
    {
      vertices [0 + i * 3] = x[i];
      vertices [1 + i * 3] = y[i];
      vertices [2 + i * 3] = 0.;
      gluTessVertex (tobj, &vertices [i * 3], &vertices [i * 3]);
    }

  gluTessEndContour (tobj);
  gluTessEndPolygon (tobj);
  gluDeleteTess (tobj);

  myFreeCombined ();
  free (vertices);
}

void
tesselate_contour (GLUtesselator *tobj, PLINE *contour, GLdouble *vertices)
{
  VNODE *vn = &contour->head;
  int offset = 0;

  gluTessBeginPolygon (tobj, NULL);
  gluTessBeginContour (tobj);
  do {
    vertices [0 + offset] = vn->point[0];
    vertices [1 + offset] = vn->point[1];
    vertices [2 + offset] = 0.;
    gluTessVertex (tobj, &vertices [offset], &vertices [offset]);
    offset += 3;
  } while ((vn = vn->next) != &contour->head);
  gluTessEndContour (tobj);
  gluTessEndPolygon (tobj);
}

struct do_hole_info {
  GLUtesselator *tobj;
  GLdouble *vertices;
};

static int
do_hole (const BoxType *b, void *cl)
{
  struct do_hole_info *info = cl;
  PLINE *curc = (PLINE *) b;

  /* Ignore the outer contour - we draw it first explicitly*/
  if (curc->Flags.orient == PLF_DIR) {
    return 0;
  }

  tesselate_contour (info->tobj, curc, info->vertices);
  return 1;
}


/* FIXME: JUST DRAWS THE FIRST PIECE.. TODO: SUPPORT FOR FULLPOLY POLYGONS */
void
hidgl_fill_pcb_polygon (PolygonType *poly, const BoxType *clip_box, double scale)
{
  int vertex_count = 0;
  PLINE *contour;
  struct do_hole_info info;

  global_scale = scale;

  if (poly->Clipped == NULL)
    {
      fprintf (stderr, "hidgl_fill_pcb_polygon: poly->Clipped == NULL\n");
      return;
    }

  /* Flush out any existing geoemtry to be rendered */
  hidgl_flush_triangles (&buffer);

  /* Walk the polygon structure, counting vertices */
  /* This gives an upper bound on the amount of storage required */
  for (contour = poly->Clipped->contours;
       contour != NULL; contour = contour->next)
    vertex_count = MAX (vertex_count, contour->Count);

  info.vertices = malloc (sizeof(GLdouble) * vertex_count * 3);
  info.tobj = gluNewTess ();
  gluTessCallback(info.tobj, GLU_TESS_BEGIN, myBegin);
  gluTessCallback(info.tobj, GLU_TESS_VERTEX, myVertex);
  gluTessCallback(info.tobj, GLU_TESS_COMBINE, myCombine);
  gluTessCallback(info.tobj, GLU_TESS_ERROR, myError);

  glClearStencil (0);
  glClear (GL_STENCIL_BUFFER_BIT);
  glColorMask (0, 0, 0, 0);                   /* Disable writting in color buffer */
  glEnable (GL_STENCIL_TEST);

  /* Drawing operations set the stencil buffer to '1' */
  glStencilFunc (GL_ALWAYS, 1, 1);            /* Test always passes, value written 1 */
  glStencilOp (GL_KEEP, GL_KEEP, GL_REPLACE); /* Stencil pass => replace stencil value (with 1) */

  r_search (poly->Clipped->contour_tree, clip_box, NULL, do_hole, &info);
  hidgl_flush_triangles (&buffer);

  /* Drawing operations as masked to areas where the stencil buffer is '1' */
  glColorMask (1, 1, 1, 1);                   /* Enable drawing of r, g, b & a */
  glStencilFunc (GL_EQUAL, 0, 1);             /* Draw only where stencil buffer is 0 */
  glStencilOp (GL_KEEP, GL_KEEP, GL_KEEP);    /* Stencil buffer read only */

  /* Draw the polygon outer */
  tesselate_contour (info.tobj, poly->Clipped->contours, info.vertices);
  hidgl_flush_triangles (&buffer);

  glClear (GL_STENCIL_BUFFER_BIT);
  glDisable (GL_STENCIL_TEST);                /* Disable Stencil test */

  gluDeleteTess (info.tobj);
  myFreeCombined ();
  free (info.vertices);
}

void
hidgl_fill_rect (int x1, int y1, int x2, int y2)
{
  hidgl_ensure_triangle_space (&buffer, 2);
  hidgl_add_triangle (&buffer, x1, y1, x1, y2, x2, y2);
  hidgl_add_triangle (&buffer, x2, y1, x2, y2, x1, y1);
}
