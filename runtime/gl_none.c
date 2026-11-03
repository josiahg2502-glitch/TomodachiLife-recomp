/* Android build only (build.py): the GL 1.1 functions of <GL/gl.h> (runtime/plat/android/GL/gl.h, Mesa), empty. The OpenGL
 * renderer in nvn_gl.c is compiled into libmain.so with the shared front end but never runs there (Vulkan only); Android binds
 * every name at load, so the calls in it need a definition. Made by a script from the header; the GL 1.2+ names nvn_gl.c
 * loads itself are renamed by plat/GL/gl.h, as everywhere. */
#include <GL/gl.h>

GLAPI void GLAPIENTRY glClearIndex(GLfloat c) { (void)c; }
GLAPI void GLAPIENTRY glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glClear(GLbitfield mask) { (void)mask; }
GLAPI void GLAPIENTRY glIndexMask(GLuint mask) { (void)mask; }
GLAPI void GLAPIENTRY glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glAlphaFunc(GLenum func, GLclampf ref) { (void)func; (void)ref; }
GLAPI void GLAPIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor) { (void)sfactor; (void)dfactor; }
GLAPI void GLAPIENTRY glLogicOp(GLenum opcode) { (void)opcode; }
GLAPI void GLAPIENTRY glCullFace(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glFrontFace(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glPointSize(GLfloat size) { (void)size; }
GLAPI void GLAPIENTRY glLineWidth(GLfloat width) { (void)width; }
GLAPI void GLAPIENTRY glLineStipple(GLint factor, GLushort pattern) { (void)factor; (void)pattern; }
GLAPI void GLAPIENTRY glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
GLAPI void GLAPIENTRY glPolygonOffset(GLfloat factor, GLfloat units) { (void)factor; (void)units; }
GLAPI void GLAPIENTRY glPolygonStipple(const GLubyte *mask) { (void)mask; }
GLAPI void GLAPIENTRY glGetPolygonStipple(GLubyte *mask) { (void)mask; }
GLAPI void GLAPIENTRY glEdgeFlag(GLboolean flag) { (void)flag; }
GLAPI void GLAPIENTRY glEdgeFlagv(const GLboolean *flag) { (void)flag; }
GLAPI void GLAPIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height; }
GLAPI void GLAPIENTRY glClipPlane(GLenum plane, const GLdouble *equation) { (void)plane; (void)equation; }
GLAPI void GLAPIENTRY glGetClipPlane(GLenum plane, GLdouble *equation) { (void)plane; (void)equation; }
GLAPI void GLAPIENTRY glDrawBuffer(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glReadBuffer(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glEnable(GLenum cap) { (void)cap; }
GLAPI void GLAPIENTRY glDisable(GLenum cap) { (void)cap; }
GLAPI GLboolean GLAPIENTRY glIsEnabled(GLenum cap) { (void)cap; return 0; }
GLAPI void GLAPIENTRY glEnableClientState(GLenum cap) { (void)cap; }
GLAPI void GLAPIENTRY glDisableClientState(GLenum cap) { (void)cap; }
GLAPI void GLAPIENTRY glGetBooleanv(GLenum pname, GLboolean *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetDoublev(GLenum pname, GLdouble *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetFloatv(GLenum pname, GLfloat *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetIntegerv(GLenum pname, GLint *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glPushAttrib(GLbitfield mask) { (void)mask; }
GLAPI void GLAPIENTRY glPopAttrib(void) { }
GLAPI void GLAPIENTRY glPushClientAttrib(GLbitfield mask) { (void)mask; }
GLAPI void GLAPIENTRY glPopClientAttrib(void) { }
GLAPI GLint GLAPIENTRY glRenderMode(GLenum mode) { (void)mode; return 0; }
GLAPI GLenum GLAPIENTRY glGetError(void) { return 0; }
GLAPI const GLubyte * GLAPIENTRY glGetString(GLenum name) { (void)name; return 0; }
GLAPI void GLAPIENTRY glFinish(void) { }
GLAPI void GLAPIENTRY glFlush(void) { }
GLAPI void GLAPIENTRY glHint(GLenum target, GLenum mode) { (void)target; (void)mode; }
GLAPI void GLAPIENTRY glClearDepth(GLclampd depth) { (void)depth; }
GLAPI void GLAPIENTRY glDepthFunc(GLenum func) { (void)func; }
GLAPI void GLAPIENTRY glDepthMask(GLboolean flag) { (void)flag; }
GLAPI void GLAPIENTRY glDepthRange(GLclampd near_val, GLclampd far_val) { (void)near_val; (void)far_val; }
GLAPI void GLAPIENTRY glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glAccum(GLenum op, GLfloat value) { (void)op; (void)value; }
GLAPI void GLAPIENTRY glMatrixMode(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) { (void)left; (void)right; (void)bottom; (void)top; (void)near_val; (void)far_val; }
GLAPI void GLAPIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) { (void)left; (void)right; (void)bottom; (void)top; (void)near_val; (void)far_val; }
GLAPI void GLAPIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height; }
GLAPI void GLAPIENTRY glPushMatrix(void) { }
GLAPI void GLAPIENTRY glPopMatrix(void) { }
GLAPI void GLAPIENTRY glLoadIdentity(void) { }
GLAPI void GLAPIENTRY glLoadMatrixd(const GLdouble *m) { (void)m; }
GLAPI void GLAPIENTRY glLoadMatrixf(const GLfloat *m) { (void)m; }
GLAPI void GLAPIENTRY glMultMatrixd(const GLdouble *m) { (void)m; }
GLAPI void GLAPIENTRY glMultMatrixf(const GLfloat *m) { (void)m; }
GLAPI void GLAPIENTRY glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) { (void)angle; (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) { (void)angle; (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glScaled(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glTranslated(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
GLAPI GLboolean GLAPIENTRY glIsList(GLuint list) { (void)list; return 0; }
GLAPI void GLAPIENTRY glDeleteLists(GLuint list, GLsizei range) { (void)list; (void)range; }
GLAPI GLuint GLAPIENTRY glGenLists(GLsizei range) { (void)range; return 0; }
GLAPI void GLAPIENTRY glNewList(GLuint list, GLenum mode) { (void)list; (void)mode; }
GLAPI void GLAPIENTRY glEndList(void) { }
GLAPI void GLAPIENTRY glCallList(GLuint list) { (void)list; }
GLAPI void GLAPIENTRY glCallLists(GLsizei n, GLenum type, const GLvoid *lists) { (void)n; (void)type; (void)lists; }
GLAPI void GLAPIENTRY glListBase(GLuint base) { (void)base; }
GLAPI void GLAPIENTRY glBegin(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glEnd(void) { }
GLAPI void GLAPIENTRY glVertex2d(GLdouble x, GLdouble y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glVertex2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glVertex2i(GLint x, GLint y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glVertex2s(GLshort x, GLshort y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glVertex3d(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glVertex3i(GLint x, GLint y, GLint z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glVertex3s(GLshort x, GLshort y, GLshort z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glVertex4i(GLint x, GLint y, GLint z, GLint w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glVertex2dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex2fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex2iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex2sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex3dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex3fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex3iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex3sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex4dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex4fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex4iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glVertex4sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz) { (void)nx; (void)ny; (void)nz; }
GLAPI void GLAPIENTRY glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz) { (void)nx; (void)ny; (void)nz; }
GLAPI void GLAPIENTRY glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) { (void)nx; (void)ny; (void)nz; }
GLAPI void GLAPIENTRY glNormal3i(GLint nx, GLint ny, GLint nz) { (void)nx; (void)ny; (void)nz; }
GLAPI void GLAPIENTRY glNormal3s(GLshort nx, GLshort ny, GLshort nz) { (void)nx; (void)ny; (void)nz; }
GLAPI void GLAPIENTRY glNormal3bv(const GLbyte *v) { (void)v; }
GLAPI void GLAPIENTRY glNormal3dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glNormal3fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glNormal3iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glNormal3sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glIndexd(GLdouble c) { (void)c; }
GLAPI void GLAPIENTRY glIndexf(GLfloat c) { (void)c; }
GLAPI void GLAPIENTRY glIndexi(GLint c) { (void)c; }
GLAPI void GLAPIENTRY glIndexs(GLshort c) { (void)c; }
GLAPI void GLAPIENTRY glIndexub(GLubyte c) { (void)c; }
GLAPI void GLAPIENTRY glIndexdv(const GLdouble *c) { (void)c; }
GLAPI void GLAPIENTRY glIndexfv(const GLfloat *c) { (void)c; }
GLAPI void GLAPIENTRY glIndexiv(const GLint *c) { (void)c; }
GLAPI void GLAPIENTRY glIndexsv(const GLshort *c) { (void)c; }
GLAPI void GLAPIENTRY glIndexubv(const GLubyte *c) { (void)c; }
GLAPI void GLAPIENTRY glColor3b(GLbyte red, GLbyte green, GLbyte blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3d(GLdouble red, GLdouble green, GLdouble blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3f(GLfloat red, GLfloat green, GLfloat blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3i(GLint red, GLint green, GLint blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3s(GLshort red, GLshort green, GLshort blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3ub(GLubyte red, GLubyte green, GLubyte blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3ui(GLuint red, GLuint green, GLuint blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor3us(GLushort red, GLushort green, GLushort blue) { (void)red; (void)green; (void)blue; }
GLAPI void GLAPIENTRY glColor4b(GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4i(GLint red, GLint green, GLint blue, GLint alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4s(GLshort red, GLshort green, GLshort blue, GLshort alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4ui(GLuint red, GLuint green, GLuint blue, GLuint alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glColor3bv(const GLbyte *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3ubv(const GLubyte *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3uiv(const GLuint *v) { (void)v; }
GLAPI void GLAPIENTRY glColor3usv(const GLushort *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4bv(const GLbyte *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4ubv(const GLubyte *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4uiv(const GLuint *v) { (void)v; }
GLAPI void GLAPIENTRY glColor4usv(const GLushort *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord1d(GLdouble s) { (void)s; }
GLAPI void GLAPIENTRY glTexCoord1f(GLfloat s) { (void)s; }
GLAPI void GLAPIENTRY glTexCoord1i(GLint s) { (void)s; }
GLAPI void GLAPIENTRY glTexCoord1s(GLshort s) { (void)s; }
GLAPI void GLAPIENTRY glTexCoord2d(GLdouble s, GLdouble t) { (void)s; (void)t; }
GLAPI void GLAPIENTRY glTexCoord2f(GLfloat s, GLfloat t) { (void)s; (void)t; }
GLAPI void GLAPIENTRY glTexCoord2i(GLint s, GLint t) { (void)s; (void)t; }
GLAPI void GLAPIENTRY glTexCoord2s(GLshort s, GLshort t) { (void)s; (void)t; }
GLAPI void GLAPIENTRY glTexCoord3d(GLdouble s, GLdouble t, GLdouble r) { (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glTexCoord3i(GLint s, GLint t, GLint r) { (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glTexCoord3s(GLshort s, GLshort t, GLshort r) { (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q) { (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glTexCoord4i(GLint s, GLint t, GLint r, GLint q) { (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q) { (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glTexCoord1dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord1fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord1iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord1sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord2dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord2fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord2iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord2sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord3dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord3fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord3iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord3sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord4dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord4fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord4iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glTexCoord4sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos2d(GLdouble x, GLdouble y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glRasterPos2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glRasterPos2i(GLint x, GLint y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glRasterPos2s(GLshort x, GLshort y) { (void)x; (void)y; }
GLAPI void GLAPIENTRY glRasterPos3d(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glRasterPos3i(GLint x, GLint y, GLint z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glRasterPos3s(GLshort x, GLshort y, GLshort z) { (void)x; (void)y; (void)z; }
GLAPI void GLAPIENTRY glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glRasterPos4i(GLint x, GLint y, GLint z, GLint w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w) { (void)x; (void)y; (void)z; (void)w; }
GLAPI void GLAPIENTRY glRasterPos2dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos2fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos2iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos2sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos3dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos3fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos3iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos3sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos4dv(const GLdouble *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos4fv(const GLfloat *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos4iv(const GLint *v) { (void)v; }
GLAPI void GLAPIENTRY glRasterPos4sv(const GLshort *v) { (void)v; }
GLAPI void GLAPIENTRY glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
GLAPI void GLAPIENTRY glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
GLAPI void GLAPIENTRY glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
GLAPI void GLAPIENTRY glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
GLAPI void GLAPIENTRY glRectdv(const GLdouble *v1, const GLdouble *v2) { (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glRectfv(const GLfloat *v1, const GLfloat *v2) { (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glRectiv(const GLint *v1, const GLint *v2) { (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glRectsv(const GLshort *v1, const GLshort *v2) { (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { (void)size; (void)type; (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) { (void)type; (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { (void)size; (void)type; (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glIndexPointer(GLenum type, GLsizei stride, const GLvoid *ptr) { (void)type; (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) { (void)size; (void)type; (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glEdgeFlagPointer(GLsizei stride, const GLvoid *ptr) { (void)stride; (void)ptr; }
GLAPI void GLAPIENTRY glGetPointerv(GLenum pname, GLvoid **params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glArrayElement(GLint i) { (void)i; }
GLAPI void GLAPIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count) { (void)mode; (void)first; (void)count; }
GLAPI void GLAPIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) { (void)mode; (void)count; (void)type; (void)indices; }
GLAPI void GLAPIENTRY glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer) { (void)format; (void)stride; (void)pointer; }
GLAPI void GLAPIENTRY glShadeModel(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glLightf(GLenum light, GLenum pname, GLfloat param) { (void)light; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glLighti(GLenum light, GLenum pname, GLint param) { (void)light; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glLightfv(GLenum light, GLenum pname, const GLfloat *params) { (void)light; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glLightiv(GLenum light, GLenum pname, const GLint *params) { (void)light; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetLightfv(GLenum light, GLenum pname, GLfloat *params) { (void)light; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetLightiv(GLenum light, GLenum pname, GLint *params) { (void)light; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glLightModelf(GLenum pname, GLfloat param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glLightModeli(GLenum pname, GLint param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glLightModelfv(GLenum pname, const GLfloat *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glLightModeliv(GLenum pname, const GLint *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glMaterialf(GLenum face, GLenum pname, GLfloat param) { (void)face; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glMateriali(GLenum face, GLenum pname, GLint param) { (void)face; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) { (void)face; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glMaterialiv(GLenum face, GLenum pname, const GLint *params) { (void)face; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) { (void)face; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetMaterialiv(GLenum face, GLenum pname, GLint *params) { (void)face; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glColorMaterial(GLenum face, GLenum mode) { (void)face; (void)mode; }
GLAPI void GLAPIENTRY glPixelZoom(GLfloat xfactor, GLfloat yfactor) { (void)xfactor; (void)yfactor; }
GLAPI void GLAPIENTRY glPixelStoref(GLenum pname, GLfloat param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glPixelTransferf(GLenum pname, GLfloat param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glPixelTransferi(GLenum pname, GLint param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values) { (void)map; (void)mapsize; (void)values; }
GLAPI void GLAPIENTRY glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint *values) { (void)map; (void)mapsize; (void)values; }
GLAPI void GLAPIENTRY glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort *values) { (void)map; (void)mapsize; (void)values; }
GLAPI void GLAPIENTRY glGetPixelMapfv(GLenum map, GLfloat *values) { (void)map; (void)values; }
GLAPI void GLAPIENTRY glGetPixelMapuiv(GLenum map, GLuint *values) { (void)map; (void)values; }
GLAPI void GLAPIENTRY glGetPixelMapusv(GLenum map, GLushort *values) { (void)map; (void)values; }
GLAPI void GLAPIENTRY glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte *bitmap) { (void)width; (void)height; (void)xorig; (void)yorig; (void)xmove; (void)ymove; (void)bitmap; }
GLAPI void GLAPIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) { (void)x; (void)y; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { (void)width; (void)height; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type) { (void)x; (void)y; (void)width; (void)height; (void)type; }
GLAPI void GLAPIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask) { (void)func; (void)ref; (void)mask; }
GLAPI void GLAPIENTRY glStencilMask(GLuint mask) { (void)mask; }
GLAPI void GLAPIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) { (void)fail; (void)zfail; (void)zpass; }
GLAPI void GLAPIENTRY glClearStencil(GLint s) { (void)s; }
GLAPI void GLAPIENTRY glTexGend(GLenum coord, GLenum pname, GLdouble param) { (void)coord; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexGenf(GLenum coord, GLenum pname, GLfloat param) { (void)coord; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexGeni(GLenum coord, GLenum pname, GLint param) { (void)coord; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexGendv(GLenum coord, GLenum pname, const GLdouble *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexGeniv(GLenum coord, GLenum pname, const GLint *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexGeniv(GLenum coord, GLenum pname, GLint *params) { (void)coord; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param) { (void)target; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexEnviv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexEnviv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param) { (void)target; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; }
GLAPI void GLAPIENTRY glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params) { (void)target; (void)level; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) { (void)target; (void)level; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)internalFormat; (void)width; (void)border; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)internalFormat; (void)width; (void)height; (void)border; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) { (void)target; (void)level; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glGenTextures(GLsizei n, GLuint *textures) { (void)n; (void)textures; }
GLAPI void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint *textures) { (void)n; (void)textures; }
GLAPI void GLAPIENTRY glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; }
GLAPI void GLAPIENTRY glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities) { (void)n; (void)textures; (void)priorities; }
GLAPI GLboolean GLAPIENTRY glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences) { (void)n; (void)textures; (void)residences; return 0; }
GLAPI GLboolean GLAPIENTRY glIsTexture(GLuint texture) { (void)texture; return 0; }
GLAPI void GLAPIENTRY glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLint border) { (void)target; (void)level; (void)internalformat; (void)x; (void)y; (void)width; (void)border; }
GLAPI void GLAPIENTRY glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) { (void)target; (void)level; (void)internalformat; (void)x; (void)y; (void)width; (void)height; (void)border; }
GLAPI void GLAPIENTRY glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width) { (void)target; (void)level; (void)xoffset; (void)x; (void)y; (void)width; }
GLAPI void GLAPIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)x; (void)y; (void)width; (void)height; }
GLAPI void GLAPIENTRY glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble *points) { (void)target; (void)u1; (void)u2; (void)stride; (void)order; (void)points; }
GLAPI void GLAPIENTRY glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat *points) { (void)target; (void)u1; (void)u2; (void)stride; (void)order; (void)points; }
GLAPI void GLAPIENTRY glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble *points) { (void)target; (void)u1; (void)u2; (void)ustride; (void)uorder; (void)v1; (void)v2; (void)vstride; (void)vorder; (void)points; }
GLAPI void GLAPIENTRY glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat *points) { (void)target; (void)u1; (void)u2; (void)ustride; (void)uorder; (void)v1; (void)v2; (void)vstride; (void)vorder; (void)points; }
GLAPI void GLAPIENTRY glGetMapdv(GLenum target, GLenum query, GLdouble *v) { (void)target; (void)query; (void)v; }
GLAPI void GLAPIENTRY glGetMapfv(GLenum target, GLenum query, GLfloat *v) { (void)target; (void)query; (void)v; }
GLAPI void GLAPIENTRY glGetMapiv(GLenum target, GLenum query, GLint *v) { (void)target; (void)query; (void)v; }
GLAPI void GLAPIENTRY glEvalCoord1d(GLdouble u) { (void)u; }
GLAPI void GLAPIENTRY glEvalCoord1f(GLfloat u) { (void)u; }
GLAPI void GLAPIENTRY glEvalCoord1dv(const GLdouble *u) { (void)u; }
GLAPI void GLAPIENTRY glEvalCoord1fv(const GLfloat *u) { (void)u; }
GLAPI void GLAPIENTRY glEvalCoord2d(GLdouble u, GLdouble v) { (void)u; (void)v; }
GLAPI void GLAPIENTRY glEvalCoord2f(GLfloat u, GLfloat v) { (void)u; (void)v; }
GLAPI void GLAPIENTRY glEvalCoord2dv(const GLdouble *u) { (void)u; }
GLAPI void GLAPIENTRY glEvalCoord2fv(const GLfloat *u) { (void)u; }
GLAPI void GLAPIENTRY glMapGrid1d(GLint un, GLdouble u1, GLdouble u2) { (void)un; (void)u1; (void)u2; }
GLAPI void GLAPIENTRY glMapGrid1f(GLint un, GLfloat u1, GLfloat u2) { (void)un; (void)u1; (void)u2; }
GLAPI void GLAPIENTRY glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2) { (void)un; (void)u1; (void)u2; (void)vn; (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2) { (void)un; (void)u1; (void)u2; (void)vn; (void)v1; (void)v2; }
GLAPI void GLAPIENTRY glEvalPoint1(GLint i) { (void)i; }
GLAPI void GLAPIENTRY glEvalPoint2(GLint i, GLint j) { (void)i; (void)j; }
GLAPI void GLAPIENTRY glEvalMesh1(GLenum mode, GLint i1, GLint i2) { (void)mode; (void)i1; (void)i2; }
GLAPI void GLAPIENTRY glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2) { (void)mode; (void)i1; (void)i2; (void)j1; (void)j2; }
GLAPI void GLAPIENTRY glFogf(GLenum pname, GLfloat param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glFogi(GLenum pname, GLint param) { (void)pname; (void)param; }
GLAPI void GLAPIENTRY glFogfv(GLenum pname, const GLfloat *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glFogiv(GLenum pname, const GLint *params) { (void)pname; (void)params; }
GLAPI void GLAPIENTRY glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer) { (void)size; (void)type; (void)buffer; }
GLAPI void GLAPIENTRY glPassThrough(GLfloat token) { (void)token; }
GLAPI void GLAPIENTRY glSelectBuffer(GLsizei size, GLuint *buffer) { (void)size; (void)buffer; }
GLAPI void GLAPIENTRY glInitNames(void) { }
GLAPI void GLAPIENTRY glLoadName(GLuint name) { (void)name; }
GLAPI void GLAPIENTRY glPushName(GLuint name) { (void)name; }
GLAPI void GLAPIENTRY glPopName(void) { }
GLAPI void GLAPIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices) { (void)mode; (void)start; (void)end; (void)count; (void)type; (void)indices; }
GLAPI void GLAPIENTRY glTexImage3D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)internalFormat; (void)width; (void)height; (void)depth; (void)border; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)width; (void)height; (void)depth; (void)format; (void)type; (void)pixels; }
GLAPI void GLAPIENTRY glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)x; (void)y; (void)width; (void)height; }
GLAPI void GLAPIENTRY glColorTable(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const GLvoid *table) { (void)target; (void)internalformat; (void)width; (void)format; (void)type; (void)table; }
GLAPI void GLAPIENTRY glColorSubTable(GLenum target, GLsizei start, GLsizei count, GLenum format, GLenum type, const GLvoid *data) { (void)target; (void)start; (void)count; (void)format; (void)type; (void)data; }
GLAPI void GLAPIENTRY glColorTableParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glColorTableParameterfv(GLenum target, GLenum pname, const GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glCopyColorSubTable(GLenum target, GLsizei start, GLint x, GLint y, GLsizei width) { (void)target; (void)start; (void)x; (void)y; (void)width; }
GLAPI void GLAPIENTRY glCopyColorTable(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width) { (void)target; (void)internalformat; (void)x; (void)y; (void)width; }
GLAPI void GLAPIENTRY glGetColorTable(GLenum target, GLenum format, GLenum type, GLvoid *table) { (void)target; (void)format; (void)type; (void)table; }
GLAPI void GLAPIENTRY glGetColorTableParameterfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetColorTableParameteriv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glBlendEquation(GLenum mode) { (void)mode; }
GLAPI void GLAPIENTRY glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) { (void)red; (void)green; (void)blue; (void)alpha; }
GLAPI void GLAPIENTRY glHistogram(GLenum target, GLsizei width, GLenum internalformat, GLboolean sink) { (void)target; (void)width; (void)internalformat; (void)sink; }
GLAPI void GLAPIENTRY glResetHistogram(GLenum target) { (void)target; }
GLAPI void GLAPIENTRY glGetHistogram(GLenum target, GLboolean reset, GLenum format, GLenum type, GLvoid *values) { (void)target; (void)reset; (void)format; (void)type; (void)values; }
GLAPI void GLAPIENTRY glGetHistogramParameterfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetHistogramParameteriv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glMinmax(GLenum target, GLenum internalformat, GLboolean sink) { (void)target; (void)internalformat; (void)sink; }
GLAPI void GLAPIENTRY glResetMinmax(GLenum target) { (void)target; }
GLAPI void GLAPIENTRY glGetMinmax(GLenum target, GLboolean reset, GLenum format, GLenum types, GLvoid *values) { (void)target; (void)reset; (void)format; (void)types; (void)values; }
GLAPI void GLAPIENTRY glGetMinmaxParameterfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetMinmaxParameteriv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glConvolutionFilter1D(GLenum target, GLenum internalformat, GLsizei width, GLenum format, GLenum type, const GLvoid *image) { (void)target; (void)internalformat; (void)width; (void)format; (void)type; (void)image; }
GLAPI void GLAPIENTRY glConvolutionFilter2D(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *image) { (void)target; (void)internalformat; (void)width; (void)height; (void)format; (void)type; (void)image; }
GLAPI void GLAPIENTRY glConvolutionParameterf(GLenum target, GLenum pname, GLfloat params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glConvolutionParameterfv(GLenum target, GLenum pname, const GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glConvolutionParameteri(GLenum target, GLenum pname, GLint params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glConvolutionParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glCopyConvolutionFilter1D(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width) { (void)target; (void)internalformat; (void)x; (void)y; (void)width; }
GLAPI void GLAPIENTRY glCopyConvolutionFilter2D(GLenum target, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height) { (void)target; (void)internalformat; (void)x; (void)y; (void)width; (void)height; }
GLAPI void GLAPIENTRY glGetConvolutionFilter(GLenum target, GLenum format, GLenum type, GLvoid *image) { (void)target; (void)format; (void)type; (void)image; }
GLAPI void GLAPIENTRY glGetConvolutionParameterfv(GLenum target, GLenum pname, GLfloat *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glGetConvolutionParameteriv(GLenum target, GLenum pname, GLint *params) { (void)target; (void)pname; (void)params; }
GLAPI void GLAPIENTRY glSeparableFilter2D(GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *row, const GLvoid *column) { (void)target; (void)internalformat; (void)width; (void)height; (void)format; (void)type; (void)row; (void)column; }
GLAPI void GLAPIENTRY glGetSeparableFilter(GLenum target, GLenum format, GLenum type, GLvoid *row, GLvoid *column, GLvoid *span) { (void)target; (void)format; (void)type; (void)row; (void)column; (void)span; }
GLAPI void GLAPIENTRY glActiveTexture(GLenum texture) { (void)texture; }
GLAPI void GLAPIENTRY glClientActiveTexture(GLenum texture) { (void)texture; }
GLAPI void GLAPIENTRY glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLint border, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)internalformat; (void)width; (void)border; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)border; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)depth; (void)border; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)xoffset; (void)width; (void)format; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const GLvoid *data) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset; (void)width; (void)height; (void)depth; (void)format; (void)imageSize; (void)data; }
GLAPI void GLAPIENTRY glGetCompressedTexImage(GLenum target, GLint lod, GLvoid *img) { (void)target; (void)lod; (void)img; }
GLAPI void GLAPIENTRY glMultiTexCoord1d(GLenum target, GLdouble s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1dv(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1f(GLenum target, GLfloat s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1fv(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1i(GLenum target, GLint s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1iv(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1s(GLenum target, GLshort s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1sv(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2dv(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2fv(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2i(GLenum target, GLint s, GLint t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2iv(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2s(GLenum target, GLshort s, GLshort t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2sv(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3dv(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3fv(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3iv(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3sv(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4dv(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4fv(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4iv(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4sv(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glLoadTransposeMatrixd(const GLdouble m[16]) { (void)m; }
GLAPI void GLAPIENTRY glLoadTransposeMatrixf(const GLfloat m[16]) { (void)m; }
GLAPI void GLAPIENTRY glMultTransposeMatrixd(const GLdouble m[16]) { (void)m; }
GLAPI void GLAPIENTRY glMultTransposeMatrixf(const GLfloat m[16]) { (void)m; }
GLAPI void GLAPIENTRY glSampleCoverage(GLclampf value, GLboolean invert) { (void)value; (void)invert; }
GLAPI void GLAPIENTRY glActiveTextureARB(GLenum texture) { (void)texture; }
GLAPI void GLAPIENTRY glClientActiveTextureARB(GLenum texture) { (void)texture; }
GLAPI void GLAPIENTRY glMultiTexCoord1dARB(GLenum target, GLdouble s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1dvARB(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1fARB(GLenum target, GLfloat s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1fvARB(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1iARB(GLenum target, GLint s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1ivARB(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord1sARB(GLenum target, GLshort s) { (void)target; (void)s; }
GLAPI void GLAPIENTRY glMultiTexCoord1svARB(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2dvARB(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2fvARB(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2iARB(GLenum target, GLint s, GLint t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2ivARB(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t) { (void)target; (void)s; (void)t; }
GLAPI void GLAPIENTRY glMultiTexCoord2svARB(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3dvARB(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3fvARB(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3ivARB(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r) { (void)target; (void)s; (void)t; (void)r; }
GLAPI void GLAPIENTRY glMultiTexCoord3svARB(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4dvARB(GLenum target, const GLdouble *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4fvARB(GLenum target, const GLfloat *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4ivARB(GLenum target, const GLint *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) { (void)target; (void)s; (void)t; (void)r; (void)q; }
GLAPI void GLAPIENTRY glMultiTexCoord4svARB(GLenum target, const GLshort *v) { (void)target; (void)v; }
GLAPI void GLAPIENTRY glBlendEquationSeparateATI(GLenum modeRGB, GLenum modeA) { (void)modeRGB; (void)modeA; }
