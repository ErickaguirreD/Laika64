/* LAIKA 64: OpenH264 includes <windows.h>; the Xbox 360 SDK's Win32 subset is <xtl.h>. OpenH264
   includes it inside extern "C" blocks, and <xtl.h> brings C++ templates (D3D, XNA math). */
#ifndef LAIKA_OPENH264_WINDOWS_H
#define LAIKA_OPENH264_WINDOWS_H
#ifdef __cplusplus
extern "C++" {
#endif
#include <xtl.h>
#ifdef __cplusplus
}
#endif
#endif
