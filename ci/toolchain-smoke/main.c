#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>

int main(void)
{
    EGLint egl_error = eglGetError();
    GLenum gl_error = glGetError();

    printf("toolchain smoke: EGL headers %d.%d, GLES2 headers, egl=0x%x gl=0x%x\n",
           EGL_VERSION_1_4, 0, (unsigned int)egl_error, (unsigned int)gl_error);
    return 0;
}
