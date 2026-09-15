"""SVG -> 8-bit coverage via librsvg + cairo through ctypes (no cairosvg here)."""
import ctypes, ctypes.util
from PIL import Image

_c = ctypes.CDLL(ctypes.util.find_library("cairo"))
_r = ctypes.CDLL(ctypes.util.find_library("rsvg-2"))

class RsvgRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                ("width", ctypes.c_double), ("height", ctypes.c_double)]

_c.cairo_image_surface_create.restype = ctypes.c_void_p
_c.cairo_image_surface_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
_c.cairo_create.restype = ctypes.c_void_p
_c.cairo_create.argtypes = [ctypes.c_void_p]
_c.cairo_image_surface_get_data.restype = ctypes.POINTER(ctypes.c_ubyte)
_c.cairo_image_surface_get_data.argtypes = [ctypes.c_void_p]
_c.cairo_image_surface_get_stride.argtypes = [ctypes.c_void_p]
_c.cairo_surface_flush.argtypes = [ctypes.c_void_p]
_c.cairo_destroy.argtypes = [ctypes.c_void_p]
_c.cairo_surface_destroy.argtypes = [ctypes.c_void_p]
_r.rsvg_handle_new_from_file.restype = ctypes.c_void_p
_r.rsvg_handle_new_from_file.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
_r.rsvg_handle_render_document.restype = ctypes.c_int
_r.rsvg_handle_render_document.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                           ctypes.POINTER(RsvgRect), ctypes.c_void_p]
CAIRO_FORMAT_ARGB32 = 0

def coverage(path, size):
    """alpha channel of the SVG rendered at size x size"""
    surf = _c.cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size)
    cr = _c.cairo_create(surf)
    h = _r.rsvg_handle_new_from_file(path.encode(), None)
    if not h:
        raise RuntimeError("rsvg cannot open " + path)
    vp = RsvgRect(0, 0, size, size)
    if not _r.rsvg_handle_render_document(h, cr, ctypes.byref(vp), None):
        raise RuntimeError("rsvg render failed " + path)
    _c.cairo_surface_flush(surf)
    stride = _c.cairo_image_surface_get_stride(surf)
    data = _c.cairo_image_surface_get_data(surf)
    buf = bytes(ctypes.cast(data, ctypes.POINTER(ctypes.c_ubyte * (stride * size))).contents)
    im = Image.frombuffer("RGBA", (size, size), buf, "raw", "BGRA", stride, 1).copy()
    _c.cairo_destroy(cr); _c.cairo_surface_destroy(surf)
    return im.split()[3]
