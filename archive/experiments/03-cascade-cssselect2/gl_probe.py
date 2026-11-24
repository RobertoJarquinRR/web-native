#!/usr/bin/env python3
"""Diagnostic: which OpenGL renderer does WGL expose on this machine?

moderngl needs OpenGL >= 3.3. If WGL only offers the Microsoft GDI Generic
1.1 renderer, `wglCreateContextAttribsARB` is missing and no modern context
can be created. Uses one throwaway window and a legacy GL 1.1 context.
"""
import ctypes
import ctypes.wintypes as wintypes


class PixelFormatDescriptor(ctypes.Structure):
    _fields_ = [
        ("nSize", wintypes.WORD), ("nVersion", wintypes.WORD),
        ("dwFlags", wintypes.DWORD), ("iPixelType", ctypes.c_ubyte),
        ("cColorBits", ctypes.c_ubyte),
    ] + [(f"reserved{i}", ctypes.c_ubyte) for i in range(18)] + [
        ("dwLayerMask", wintypes.DWORD), ("dwVisibleMask", wintypes.DWORD),
        ("dwDamageMask", wintypes.DWORD),
    ]


user32, gdi32, opengl32 = (ctypes.windll.user32, ctypes.windll.gdi32,
                           ctypes.windll.opengl32)
pfd = PixelFormatDescriptor()
pfd.nSize, pfd.nVersion = ctypes.sizeof(pfd), 1
pfd.dwFlags = 0x4 | 0x20 | 0x1  # DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
pfd.iPixelType, pfd.cColorBits, pfd.cDepthBits = 0, 32, 24

user32.CreateWindowExW.restype = wintypes.HWND
hwnd = user32.CreateWindowExW(0, "STATIC", "glprobe", 0x10000000,
                              0, 0, 64, 64, None, None, None, None)
hdc = user32.GetDC(hwnd)
try:
    fmt = gdi32.ChoosePixelFormat(hdc, ctypes.byref(pfd))
    print("pixel format:", fmt, "set:", gdi32.SetPixelFormat(hdc, fmt, ctypes.byref(pfd)))
    opengl32.wglCreateContext.restype = ctypes.c_void_p
    hglrc = opengl32.wglCreateContext(hdc)
    print("legacy context:", hglrc,
          "make current:", opengl32.wglMakeCurrent(hdc, ctypes.c_void_p(hglrc)))
    opengl32.glGetString.restype = ctypes.c_char_p
    print("GL_VENDOR  :", opengl32.glGetString(0x1F00))
    print("GL_RENDERER:", opengl32.glGetString(0x1F01))
    print("GL_VERSION :", opengl32.glGetString(0x1F02))
    opengl32.wglGetProcAddress.restype = ctypes.c_void_p
    print("wglCreateContextAttribsARB:",
          opengl32.wglGetProcAddress(b"wglCreateContextAttribsARB"))
finally:
    user32.ReleaseDC(hwnd, hdc)
