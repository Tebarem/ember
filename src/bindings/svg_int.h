#pragma once
/*
 * Internals shared by impl_svg.c (the elements and the geometry hook), impl_svg_val.c (the values) and impl_svg_list.c
 * (the lists and the animated wrappers) of the SVG DOM (R4d). Private to the bindings module.
 */
#include "bindings/impl.h"
#include "bindings/gen/geometry.h"
#include "bindings/gen/svg.h"

/* The attributes behind the animated values and the lists: the index is the aux of a view. */
enum {
    SA_class,
    SA_href,
    SA_target,
    SA_x,
    SA_y,
    SA_width,
    SA_height,
    SA_rx,
    SA_ry,
    SA_cx,
    SA_cy,
    SA_r,
    SA_fx,
    SA_fy,
    SA_fr,
    SA_x1,
    SA_y1,
    SA_x2,
    SA_y2,
    SA_offset,
    SA_pathLength,
    SA_viewBox,
    SA_preserveAspectRatio,
    SA_refX,
    SA_refY,
    SA_markerWidth,
    SA_markerHeight,
    SA_markerUnits,
    SA_orient,
    SA_orientType,
    SA_dx,
    SA_dy,
    SA_rotate,
    SA_textLength,
    SA_lengthAdjust,
    SA_startOffset,
    SA_method,
    SA_spacing,
    SA_side,
    SA_points,
    SA_animatedPoints,
    SA_transform,
    SA_gradientTransform,
    SA_patternTransform,
    SA_requiredExtensions,
    SA_systemLanguage,
    SA_clipPathUnits,
    SA_gradientUnits,
    SA_spreadMethod,
    SA_maskUnits,
    SA_maskContentUnits,
    SA_patternUnits,
    SA_patternContentUnits,
    SA_COUNT
};

/* The kind of a value payload. */
enum {
    SV_NUM = 1, /* SVGNumber: u.num */
    SV_LEN,     /* SVGLength: u.num in unit */
    SV_ANGLE,   /* SVGAngle: u.num in unit (SV_ANGLE_*) */
    SV_PT,      /* SVGPoint, DOMPoint, DOMPointReadOnly: u.r = x, y, z, w */
    SV_RECT,    /* SVGRect, DOMRect, DOMRectReadOnly: u.r = x, y, width, height */
    SV_MX,      /* SVGMatrix, DOMMatrix, DOMMatrixReadOnly: u.mx (column-major, m11 m12 m13 m14 m21 ...) */
    SV_TF,      /* SVGTransform */
    SV_PAR,     /* SVGPreserveAspectRatio */
    SV_LIST,    /* a list view (SVG*List) */
    SV_STR      /* the items of an SVGStringList (ikind only) */
};

/* Where the data of a value lives. */
enum {
    SB_FREE = 0, /* the payload is the value */
    SB_ATTR,     /* an attribute of the element in ref (attr names it) */
    SB_ITEM,     /* an item of the list in ref */
    SB_MATRIX    /* the matrix of the SVGTransform in ref */
};

enum { SVG_LT_NUMBER = 1, SVG_LT_PERCENTAGE, SVG_LT_EMS, SVG_LT_EXS, SVG_LT_PX, SVG_LT_CM, SVG_LT_MM, SVG_LT_IN, SVG_LT_PT,
       SVG_LT_PC, SVG_LT_COUNT };
enum { SVG_AT_UNSPECIFIED = 1, SVG_AT_DEG, SVG_AT_RAD, SVG_AT_GRAD, SVG_AT_TURN /* unitType UNKNOWN */ };
enum { SVG_TF_MATRIX = 1, SVG_TF_TRANSLATE, SVG_TF_SCALE, SVG_TF_ROTATE, SVG_TF_SKEWX, SVG_TF_SKEWY };

/* The payload of an SVG value object (the `owned` memory of its record, or, for a value, also its ptr). A value is
 * allocated for its kind only (impl_svg_val.c sv_size): the union is as large as the matrix, a DOMRect or a point is not. */
typedef struct sv {
    embi_bind_obj *self;
    uint8_t kind;
    uint8_t ro;    /* animVal: writes throw NoModificationAllowedError */
    uint8_t bound; /* SB_* */
    uint8_t ikind; /* lists: the kind of the items */
    uint16_t attr; /* SA_* of the attribute (bound values and lists) */
    uint16_t unit; /* SV_LEN, SV_ANGLE */
    union {
        double num;
        double r[4];
        struct {
            double m[16];
            bool is2d;
        } mx;
        struct {
            double m[6];
            double angle, cx, cy;
            uint8_t type;
        } tf;
        struct {
            uint8_t align, mos;
        } par;
        struct {
            char *last; /* the attribute text the items were parsed from */
            uint32_t last_len;
            uint32_t n; /* the number of items (the length of ref, the array of item wrappers) */
            uint32_t epoch; /* the mutation epoch of the context at which the items were last in step with the attribute */
            bool have;
        } list;
    } u;
} sv;

/* ------------------------------------------------------------------ impl_svg_val.c */

/* A new payload and its wrapper of interface iface (kind SV_*); *out receives the wrapper. */
sv *embi_sv_new(embi_bind_ctx *c, uint16_t iface, uint8_t kind, embi_js_value *out);
/* Refreshes the payload from the attribute or the list it is bound to, and writes it back. */
void embi_sv_load(embi_bind_ctx *c, sv *p);
void embi_sv_store(embi_bind_ctx *c, sv *p);
/* The data of src into dst (the binding of dst stays). */
void embi_sv_copy_data(sv *dst, const sv *src);
/* The interface (EMBI_IF_*) of an item kind. */
uint16_t embi_sv_iface_of(uint8_t kind);
/* Parses the text of a list attribute into temporary items (an embi_alloc'd array of *n full-size payloads with room for
 * *cap, nullptr when n == 0; free it with embi_sv_free_items(items, cap)). False when the text is invalid (no items). */
bool embi_sv_parse_list(embi_str text, uint8_t kind, uint32_t attr, sv **items, uint32_t *n, uint32_t *cap);
void embi_sv_free_items(sv *items, uint32_t cap);
/* Appends the attribute text of one item. */
void embi_sv_serialize_item(struct embi_sb *sb, const sv *p);
/* The text of an attribute of the element, false when absent. */
bool embi_sv_attr_text(embi_bind_ctx *c, embi_node *n, uint32_t idx, embi_str *out);
void embi_sv_set_attr(embi_bind_ctx *c, embi_node *n, uint32_t idx, embi_str text);
const char *embi_sv_attr_name(uint32_t idx);
/* The record that backs the parent of a bound value (nullptr when none or stale). */
embi_bind_obj *embi_sv_parent(const sv *p);
/* True (and throws a TypeError with the message) when v is outside the range of a float. */
bool embi_sv_float_range_error(embi_bind_ctx *c, double v);

/* ------------------------------------------------------------------ impl_svg_list.c */

/* Re-parses the list when its attribute changed, and writes it back. */
void embi_svg_list_sync(embi_bind_ctx *c, embi_bind_obj *list);
void embi_svg_list_commit(embi_bind_ctx *c, embi_bind_obj *list);
/* The wrapper of an animated value or list of the element self: the attribute is idx (SA_*), the interface iface. */
embi_js_value embi_svg_anim_view(embi_bind_ctx *c, embi_node *self, uint16_t iface, uint32_t idx);
/* A list view over the attribute idx of the element self (SameObject-cached by the caller). */
embi_js_value embi_svg_list_view(embi_bind_ctx *c, embi_node *self, uint16_t iface, uint32_t idx, bool ro);

/* ------------------------------------------------------------------ values for the elements (impl_svg_val.c) */

embi_js_value embi_svg_dommatrix_new(embi_bind_ctx *c, const double m[6]);
embi_js_value embi_svg_dompoint_new(embi_bind_ctx *c, double x, double y, double z, double w);
/* A new SVGPoint (currentTranslate, getPointAtLength): the type of the points of an SVG list, writable. */
embi_js_value embi_svg_svgpoint_new(embi_bind_ctx *c, double x, double y);
embi_js_value embi_svg_new_value(embi_bind_ctx *c, uint16_t iface, uint8_t kind);
/* An SVGTransform of type matrix from a DOMMatrix2DInit (a DOMMatrix or SVGMatrix passes as one). EMBI_JS_EXCEPTION, with a
 * TypeError pending, when an alias and its element disagree. */
embi_js_value embi_svg_new_transform_from_init(embi_bind_ctx *c, const embi_dict_DOMMatrix2DInit *init);
/* The members of a DOMMatrix2DInit as a 4x4 matrix; false (a TypeError is pending) when an alias and its element disagree. */
bool embi_svg_matrix_from_init(embi_bind_ctx *c, const embi_dict_DOMMatrix2DInit *d, double m[16]);
