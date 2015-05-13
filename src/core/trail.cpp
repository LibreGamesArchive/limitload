#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#endif
#include <cmath>

#include <boundingSphere.h>
#include <geomVertexFormat.h>
#include <geomVertexArrayFormat.h>
#include <geomVertexData.h>
#include <geomVertexWriter.h>
#include <geomVertexRewriter.h>
#include <lvector2.h>
#include <transparencyAttrib.h>

#include <trail.h>
#include <misc.h>

static void _init_trail ()
{
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;
    INITIALIZE_TYPE(PolyTrailGeom)
    INITIALIZE_TYPE(PolyExhaustGeom)
    INITIALIZE_TYPE(PolyBraidGeom)
}
DToolConfigure(config_limload_trail);
DToolConfigureFn(config_limload_trail) { _init_trail(); }

// ========================================
// PolyTrailGeom

class TrailPoint
{
public:
    double ctime;
    LPoint3 apos;
};
typedef std::deque<TrailPoint*>::iterator TrailPointsIter;

INITIALIZE_TYPE_HANDLE(PolyTrailGeom)

PolyTrailGeom::PolyTrailGeom (
    int numpoly, double randcircle, double segperiod,
    const LPoint3 &apos, const LQuaternion& aquat,
    const NodePath &pnode)
: _randcircle(randcircle)
, _segperiod(segperiod)
, _maxpoly(numpoly)
, _prev_apos(apos)
, _prev_aquat(aquat)
, _points()
, _numpoints(0)
{
    _gvdata = new GeomVertexData("trail", GeomVertexFormat::get_v3n3c4t2(), Geom::UH_static);//UH_dynamic);
    GeomVertexWriter *gvwvertex = new GeomVertexWriter(_gvdata, "vertex");
    GeomVertexWriter *gvwnormal = new GeomVertexWriter(_gvdata, "normal");
    GeomVertexWriter *gvwtexcoord = new GeomVertexWriter(_gvdata, "texcoord");
    GeomVertexWriter *gvwcolor = new GeomVertexWriter(_gvdata, "color");
    _gtris = new GeomTriangles(Geom::UH_static);
    for (int i = 0; i < _maxpoly; ++i) {
        for (int j = 0; j < 3; ++j) {
            LVector2 v2 = LVector2(0.0, fx_uniform0());
            LVector3 v3 = LVector3(0.0, 0.0, fx_uniform0());
            LVector4 v4 = LVector4(1.0, 1.0, 1.0, fx_uniform0());
            gvwvertex->add_data3(v3);
            gvwnormal->add_data3(v3);
            gvwtexcoord->add_data2(v2);
            gvwcolor->add_data4(v4);
        }
        _gtris->add_vertices(i * 3, i * 3 + 1, i * 3 + 2);
    }
    _gtris->close_primitive();
    _geom = new Geom(_gvdata);
    _geom->add_primitive(_gtris);
    _geomnode = new GeomNode("trail-geom");
    _geomnode->add_geom(_geom);
    _node = NodePath("trail");
    _node.attach_new_node(_geomnode);
    delete gvwvertex;
    delete gvwnormal;
    delete gvwtexcoord;
    delete gvwcolor;
    _last_clear_index = _maxpoly;
    _node.set_depth_write(0);
    _node.set_transparency(TransparencyAttrib::M_alpha);
    _node.reparent_to(pnode);
}

PolyTrailGeom::~PolyTrailGeom ()
{
    //printf("--polytrailgeom-dtr\n");
    for (TrailPointsIter it = _points.begin(); it != _points.end(); ++it) {
        delete *it;
    }
    _node.remove_node();
}

void PolyTrailGeom::update (
    const NodePath &camera, double lifespan, double lodalfac,
    double radius0, double radius1,
    const LVector4 &color_, const LVector4 &endcolor, double tcol,
    const LPoint3 &bpos,
    bool havepq, const LPoint3 &apos_, const LQuaternion& aquat_,
    double adt)
{
    LPoint3 apos = apos_;
    LQuaternion aquat = aquat_;

    while (_numpoints > 0 && _points.back()->ctime >= lifespan) {
        delete _points.back();
        _points.pop_back();
        --_numpoints;
    }
    if (havepq) {
        if (apos != _prev_apos) {
            LPoint3 ap = apos;
            if (_randcircle > 0.0) {
                LVector3 aup = aquat.get_up();
                LVector3 art = aquat.get_right();
                double dang = fx_uniform2(0.0, 2 * M_PI);
                double drad = fx_uniform2(0.0, _randcircle);
                ap += vrot(art, aup, drad, dang);
            }
            _points.push_front(new TrailPoint());
            TrailPoint &point = *_points.front();
            point.ctime = 0.0;
            point.apos = ap;
            ++_numpoints;
            _prev_apos = apos;
        }
    } else if (_points.empty()) {
        return;
    }

    _node.set_pos(bpos);

    LVector4 color0 = color_;
    LVector4 color1 = endcolor;
    if (lodalfac > 0.0) {
        color0[3] *= lodalfac;
        color1[3] *= lodalfac;
    }
    double tfac = tcol;

    double u0 = 0.0;
    double v0 = 0.0;
    double du = 1.0;
    double dv = 1.0;

    GeomVertexRewriter *gvwvertex = new GeomVertexRewriter(_gvdata, "vertex");
    GeomVertexRewriter *gvwnormal = new GeomVertexRewriter(_gvdata, "normal");
    GeomVertexRewriter *gvwtexcoord = new GeomVertexRewriter(_gvdata, "texcoord");
    GeomVertexRewriter *gvwcolor = new GeomVertexRewriter(_gvdata, "color");
    _gtris->decompose();

    const int quads_per_seg = 2;
    LVector3 cp = camera.get_pos(_node); // _node must be at bpos
    const int tris_per_seg = quads_per_seg * 2;
    int clear_index = 0;
    double maxreach = 0.0;
    double rad0;
    LVector3 p0, n0, r0;
    LVector4 col0;
    for (int i = 0; i < _numpoints; ++i) {
        TrailPoint &point1 = *_points[i];

        double ifac1 = point1.ctime / lifespan;

        double rad1 = radius0 + (radius1 - radius0) * ifac1;

        LVector4 col1;
        if (ifac1 < tfac) {
            col1 = color0 + (color1 - color0) * (ifac1 / tfac);
        } else {
            col1 = color1;
        }
        col1[3] *= (1.0 - ifac1);

        LVector3 p1 = point1.apos - bpos;

        if (i > 0) {
            LVector3 u01 = unitv(p0 - p1);
            if (i == 1) {
                LVector3 uc0 = unitv(cp - p0);
                n0 = unitv(u01.cross(uc0).cross(u01));
                r0 = u01.cross(n0);
            }
            LVector3 uc1 = unitv(cp - p1);
            LVector3 n1 = unitv(u01.cross(uc1).cross(u01));
            LVector3 r1 = u01.cross(n1);
            LVector3 qp0a = p0 - r0 * rad0;
            LVector3 qp0b = p0 + r0 * rad0;
            LVector3 qp1a = p1 - r1 * rad1;
            LVector3 qp1b = p1 + r1 * rad1;
            LVector3 nrm0a = unitv(-r0 + n0 * 0.1);
            LVector3 nrm0b = unitv(r0 + n0 * 0.1);
            LVector3 nrm1a = unitv(-r1 + n1 * 0.1);
            LVector3 nrm1b = unitv(r1 + n1 * 0.1);

            if (clear_index + tris_per_seg <= _maxpoly) {
                add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                        qp0a, nrm0a, col0, LVector2(u0, v0),
                        qp1a, nrm1a, col1, LVector2(u0 + du, v0),
                        p1, n1, col1, LVector2(u0 + du, v0 + dv * 0.5));
                add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                        p1, n1, col1, LVector2(u0 + du, v0 + dv * 0.5),
                        p0, n0, col0, LVector2(u0, v0 + dv * 0.5),
                        qp0a, nrm0a, col0, LVector2(u0, v0));
                add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                        p0, n0, col0, LVector2(u0, v0 + dv * 0.5),
                        p1, n1, col1, LVector2(u0 + du, v0 + dv * 0.5),
                        qp1b, nrm1b, col1, LVector2(u0 + du, v0 + dv));
                add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                        qp1b, nrm1b, col1, LVector2(u0 + du, v0 + dv),
                        qp0b, nrm0b, col0, LVector2(u0, v0 + dv),
                        p0, n0, col0, LVector2(u0, v0 + dv * 0.5));
                clear_index += tris_per_seg;
                maxreach = fmax(maxreach, p0.length());
            }

            n0 = n1; r0 = r1;
        }
        point1.ctime += adt;
        p0 = p1;
        rad0 = rad1; col0 = col1;
    }

    for(int i = clear_index; i < _last_clear_index; ++i) {
        for (int j = 0; j < 3; ++j) {
            gvwvertex->add_data3(0.0, 0.0, 0.0);
        }
    }
    _last_clear_index = clear_index;
    if (_numpoints > 1) {
        LPoint3 center(0.0, 0.0, 0.0);
        PT(BoundingSphere) bnd = new BoundingSphere(center, maxreach);
        _node.node()->set_bounds(bnd);
        _node.node()->set_final(1);
    }

    delete gvwvertex;
    delete gvwnormal;
    delete gvwtexcoord;
    delete gvwcolor;
}

void PolyTrailGeom::clear (
    const NodePath &camera,
    bool havepq, const LPoint3 &apos, const LQuaternion& aquat)
{
    for (TrailPointsIter it = _points.begin(); it != _points.end(); ++it) {
        delete *it;
    }
    _points.clear();
    _numpoints = 0;

    GeomVertexRewriter *gvwvertex = new GeomVertexRewriter(_gvdata, "vertex");
    _gtris->decompose();
    for(int i = 0; i < _last_clear_index; ++i) {
        for (int j = 0; j < 3; ++j) {
            gvwvertex->add_data3(0.0, 0.0, 0.0);
        }
    }
    _last_clear_index = 0;
    delete gvwvertex;

    if (havepq) {
        _prev_apos = apos;
        _prev_aquat = aquat;
    }
}

NodePath PolyTrailGeom::node ()
{
    return _node;
}

bool PolyTrailGeom::any_visible () const
{
    return _numpoints > 1;
}

double PolyTrailGeom::seg_period () const
{
    return _segperiod;
}

LPoint3 PolyTrailGeom::prev_apos () const
{
    return _prev_apos;
}

LQuaternion PolyTrailGeom::prev_aquat () const
{
    return _prev_aquat;
}

void PolyTrailGeom::set_prev (const LPoint3 &apos, const LQuaternion& aquat)
{
    _prev_apos = apos;
    _prev_aquat = aquat;
}

// ========================================
// PolyExhaustGeom

class ExhaustParticle
{
public:
    LVector3 pos0;
    double dist;
    double ctime;
};
typedef std::deque<ExhaustParticle*>::iterator ExhPartsIter;

INITIALIZE_TYPE_HANDLE(PolyExhaustGeom)

PolyExhaustGeom::PolyExhaustGeom (const NodePath &pnode, int poolsize)
: _poolsize(poolsize)
, _gen()
, _emittime(0.0)
, _emskip_count(0)
, _particles()
, _numparts(0)
{
    _gen.set_budget(_poolsize * 4);
    NodePath gnode = _gen.get_root();
    gnode.set_depth_write(0);
    gnode.set_transparency(TransparencyAttrib::M_alpha);
    //gnode.set_attrib(ColorBlendAttrib.make(ColorBlendAttrib::M_add));
    gnode.set_two_sided(true);
    gnode.reparent_to(pnode);
}

PolyExhaustGeom::~PolyExhaustGeom ()
{
    //printf("--polyexhaustgeom-dtr\n");
    for (ExhPartsIter it = _particles.begin(); it != _particles.end(); ++it) {
        delete *it;
    }
}

bool PolyExhaustGeom::update (
    const NodePath &camera,
    bool palive, bool sigend, const LVector3 &pdir,
    double lifespan, double speed, double emradius,
    double radius0, double radius1,
    const LVector4 &color0, const LVector4 &color1, double tcol,
    const LVector3 &dbpos, int emskip,
    double adt)
{
    LVector3 ndir(0.0, 0.0, 0.0);
    if (emradius > 0.0) {
        ndir = pdir.cross(LVector3(0.0, 0.0, 1.0));
        if (ndir.normalize() == 0.0) {
            ndir = LVector3(1.0, 0.0, 0.0);
        }
    }

    if (palive && !sigend) {
        _emittime += adt;
        while (_emittime >= 0.0) {
            LVector3 pos0(0.0, 0.0, 0.0);
            if (emradius > 0.0) {
                double ang = fx_uniform2(0.0, 2 * M_PI);
                LQuaternion q;
                q.set_from_axis_angle_rad(ang, pdir);
                double rad = fx_uniform2(0.0, emradius);
                pos0 += LVector3(q.xform(ndir)) * rad;
            }
            ++_emskip_count;
            if (_emskip_count >= emskip) {
                _emskip_count = 0;
                double dist = speed * _emittime;
                dist -= speed * adt; // will be added below
                _particles.push_front(new ExhaustParticle());
                ExhaustParticle &particle = *_particles.front();
                particle.pos0 = pos0;
                particle.dist = dist;
                particle.ctime = _emittime;
                ++_numparts;

            }
            double birthrate = lifespan / _poolsize;
            _emittime -= birthrate;
        }
    } else if (_particles.empty()) {
        return false;
    }

    double tfac0 = 0.0;
    double tfac1 = tcol;
    LVector4 dcolor = color1 - color0;
    _gen.begin(camera, _gen.get_root());
    for (ExhPartsIter it = _particles.begin(); it != _particles.end(); ++it) {
        ExhaustParticle &particle = **it;
        if (particle.ctime >= lifespan) {
            for (ExhPartsIter it2 = it; it2 != _particles.end(); ++it2) {
                delete *it2;
                --_numparts;
            }
            _particles.erase(it, _particles.end());
            break;
        }
        double ifac = clamp(particle.ctime / lifespan, 0.0, 1.0);
        double ifac1 = clamp((ifac - tfac0) / (tfac1 - tfac0), 0.0, 1.0);
        LVector4 color = color0 + dcolor * ifac1;
        color[3] = color0[3] * (1.0 - ifac);
        double radius = radius0 + (radius1 - radius0) * ifac;
        LVector4 frame(0.0, 0.0, 1.0, 1.0);
        LVector3 pos = particle.pos0 + pdir * particle.dist - dbpos;
        _gen.billboard(pos, frame, radius, color);
        particle.ctime += adt;
        particle.dist += speed * adt;
    }
    _gen.end();

    if (_numparts > 0) {
        LPoint3 center(0.0, 0.0, 0.0);
        double maxreach = speed * lifespan * 1.2;
        PT(BoundingSphere) bnd = new BoundingSphere(center, maxreach);
        _gen.get_root().node()->set_bounds(bnd);
        _gen.get_root().node()->set_final(1);
    }

    return true;
}

int PolyExhaustGeom::num_particles () const
{
    return _numparts;
}

LVector3 PolyExhaustGeom::chord (const LVector3 &pdir) const
{
    ExhaustParticle &p0 = *_particles.front();
    ExhaustParticle &p1 = *_particles.back();
    LPoint3 pos0 = p0.pos0 + pdir * p0.dist;
    LPoint3 pos1 = p1.pos0 + pdir * p1.dist;
    LVector3 chord = pos0 - pos1;
    return chord;
}

// ========================================
// PolyBraidGeom

class StrandParticle
{
public:
    LPoint3 apos;
    double ctime;
};
typedef std::deque<StrandParticle*>::iterator StrandParticlesIter;

struct BraidStrand
{
public:
    double thickness;
    double endthickness;
    double spacing;
    double offang;
    double offrad;
    double offtang;
    double randang;
    double randrad;
    LVector4 color;
    LVector4 endcolor;
    double tcol;
    double alphaexp;
    int texsplit;
    int numframes;

    int maxpoly;
    NodePath node;
    PT(Geom) geom;
    PT(GeomNode) geomnode;
    PT(GeomVertexData) gvdata;
    PT(GeomTriangles) gtris;
    int last_clear_index;

    double dtang;
    double init_dtang;
    double ctang;
    double ctime;
    double dang0;
    double drad0;

    std::deque<StrandParticle*> parts;
    int numparts;

    ~BraidStrand ();
};
typedef std::deque<BraidStrand*>::iterator BraidStrandsIter;

BraidStrand::~BraidStrand ()
{
    //printf("--polybraidgeom-braidstrand-dtr\n");
    for (StrandParticlesIter it = parts.begin(); it != parts.end(); ++it) {
        delete *it;
    }
    node.remove_node();
}

INITIALIZE_TYPE_HANDLE(PolyBraidGeom)

PolyBraidGeom::PolyBraidGeom (
    double segperiod,
    const LPoint3 &apos, const LQuaternion& aquat)
: _strands()
, _segperiod(segperiod)
, _prev_apos(apos)
, _prev_aquat(aquat)
, _start_point(0.0, 0.0, 0.0)
, _end_point(0.0, 0.0, 0.0)
, _any_visible(false)
{
}

PolyBraidGeom::~PolyBraidGeom ()
{
    //printf("--polybraidgeom-dtr\n");
    for (BraidStrandsIter it = _strands.begin(); it != _strands.end(); ++it) {
        delete *it;
    }
}

NodePath PolyBraidGeom::add_strand (
    double thickness, double endthickness, double spacing,
    double offang, double offrad, double offtang,
    double randang, double randrad,
    const LVector4 &color, const LVector4 &endcolor,
    double tcol, double alphaexp,
    int texsplit, int numframes,
    int maxpoly, const NodePath &pnode)
{
    if (numframes == 0) {
        numframes = texsplit * texsplit;
    }

    _strands.push_back(new BraidStrand());
    BraidStrand &strand = *_strands.back();

    strand.thickness = thickness;
    strand.endthickness = endthickness;
    strand.spacing = spacing;
    strand.offang = offang;
    strand.offrad = offrad;
    strand.offtang = offtang;
    strand.randang = randang;
    strand.randrad = randrad;
    strand.color = color;
    strand.endcolor = endcolor;
    strand.tcol = tcol;
    strand.alphaexp = alphaexp;
    strand.texsplit = texsplit;
    strand.numframes = numframes;
    strand.maxpoly = maxpoly;

    strand.dtang = strand.thickness * strand.spacing * 2;
    strand.init_dtang = strand.dtang;
    strand.ctang = -strand.offtang * strand.dtang;
    strand.ctime = 0.0;
    strand.dang0 = torad(strand.offang);
    strand.drad0 = strand.offrad;
    strand.numparts = 0;

    strand.gvdata = new GeomVertexData("strand", GeomVertexFormat::get_v3n3c4t2(), Geom::UH_static);//UH_dynamic);
    GeomVertexWriter *gvwvertex = new GeomVertexWriter(strand.gvdata, "vertex");
    GeomVertexWriter *gvwnormal = new GeomVertexWriter(strand.gvdata, "normal");
    GeomVertexWriter *gvwtexcoord = new GeomVertexWriter(strand.gvdata, "texcoord");
    GeomVertexWriter *gvwcolor = new GeomVertexWriter(strand.gvdata, "color");
    strand.gtris = new GeomTriangles(Geom::UH_static);
    for (int i = 0; i < maxpoly; ++i) {
        for (int j = 0; j < 3; ++j) {
            LVector2 v2 = LVector2(0.0, fx_uniform0());
            LVector3 v3 = LVector3(0.0, 0.0, fx_uniform0());
            LVector4 v4 = LVector4(1.0, 1.0, 1.0, fx_uniform0());
            gvwvertex->add_data3(v3);
            gvwnormal->add_data3(v3);
            gvwtexcoord->add_data2(v2);
            gvwcolor->add_data4(v4);
        }
        strand.gtris->add_vertices(i * 3, i * 3 + 1, i * 3 + 2);
    }
    strand.gtris->close_primitive();
    strand.geom = new Geom(strand.gvdata);
    strand.geom->add_primitive(strand.gtris);
    strand.geomnode = new GeomNode("braid-strand-geom");
    strand.geomnode->add_geom(strand.geom);
    strand.node = NodePath("braid-strand");
    strand.node.attach_new_node(strand.geomnode);
    delete gvwvertex;
    delete gvwnormal;
    delete gvwtexcoord;
    delete gvwcolor;
    strand.last_clear_index = maxpoly;
    strand.node.set_depth_write(0);
    strand.node.set_transparency(TransparencyAttrib::M_alpha);
    strand.node.reparent_to(pnode);

    return strand.node;
}

void PolyBraidGeom::update (
    const NodePath &camera,
    double lifespan, double lodalfac, const LPoint3 &bpos,
    bool havepq, const LPoint3 &apos_, const LQuaternion &aquat_,
    double adt)
{
    LPoint3 apos = apos_;
    LQuaternion aquat = aquat_;
    if (havepq) {
        if (apos != _prev_apos) {
            LVector3 dpos = apos - _prev_apos;
            double maxctang = dpos.length();
            LVector3 tdir = unitv(dpos);
            LVector3 aup = aquat.get_up();
            LVector3 art = aquat.get_right();
            for (BraidStrandsIter it = _strands.begin(); it != _strands.end(); ++it) {
                BraidStrand &strand = **it;
                double ctang = strand.ctang;
                double ctime = strand.ctime;
                double dtime = strand.dtang / (maxctang / adt);
                while (ctang + strand.dtang < maxctang) {
                    ctang += strand.dtang;
                    ctime += dtime;
                    if (ctang < 0.0) { // can happen due to base offset
                        continue;
                    }
                    LPoint3 appos = _prev_apos + tdir * ctang;
                    double dang = strand.dang0;
                    if (strand.randang) {
                        dang = fx_uniform2(0.0, 2 * M_PI);
                    }
                    double drad = strand.drad0;
                    if (strand.randrad) {
                        drad = fx_uniform2(0.0, strand.drad0);
                    }
                    appos += vrot(art, aup, drad, dang);
                    strand.parts.push_front(new StrandParticle());
                    strand.numparts += 1;
                    StrandParticle &spart = *strand.parts.front();
                    spart.apos = appos;
                    spart.ctime = ctime;
                }
                ctang -= maxctang;
                ctime -= adt;
                strand.ctang = ctang;
                strand.ctime = ctime;
            }
            _prev_apos = apos;
            _prev_aquat = aquat;
        }
    } else if (_any_visible) {
        apos = _prev_apos;
        aquat = _prev_aquat;
    } else {
        return;
    }

    _any_visible = false;
    for (BraidStrandsIter it = _strands.begin(); it != _strands.end(); ++it) {
        BraidStrand &strand = **it;
        strand.node.set_pos(bpos);

        LVector3 up = strand.node.get_relative_vector(camera, LVector3(0.0, 0.0, 1.0));
        LVector3 rt = strand.node.get_relative_vector(camera, LVector3(1.0, 0.0, 0.0));
        LVector3 oc1 = -rt - up;
        LVector3 oc2 =  rt - up;
        LVector3 oc3 =  rt + up;
        LVector3 oc4 = -rt + up;
        LVector3 nrm = unitv(rt.cross(up)); // -fw
        LVector3 nrm1 = unitv(oc1 + nrm * 0.05);
        LVector3 nrm2 = unitv(oc2 + nrm * 0.05);
        LVector3 nrm3 = unitv(oc3 + nrm * 0.05);
        LVector3 nrm4 = unitv(oc4 + nrm * 0.05);

        GeomVertexRewriter *gvwvertex = new GeomVertexRewriter(strand.gvdata, "vertex");
        GeomVertexRewriter *gvwnormal = new GeomVertexRewriter(strand.gvdata, "normal");
        GeomVertexRewriter *gvwtexcoord = new GeomVertexRewriter(strand.gvdata, "texcoord");
        GeomVertexRewriter *gvwcolor = new GeomVertexRewriter(strand.gvdata, "color");
        strand.gtris->decompose();

        LVector4 col0 = strand.color;
        LVector4 col1 = strand.endcolor;
        if (lodalfac > 0.0) {
            col0[3] *= lodalfac;
            col0[3] *= lodalfac;
        }

        const int poly_per_part = 2;
        int clear_index = 0;
        int numparts = strand.numparts;
        double maxreach = 0.0;
        int i = 0;
        while (i < numparts) {
            StrandParticle &spart = *strand.parts[i];
            if (spart.ctime < lifespan) {
                double ifac = spart.ctime / lifespan;
                if (clear_index + poly_per_part <= strand.maxpoly) {
                    LPoint3 pos = spart.apos - bpos;

                    double thck0 = strand.thickness;
                    double thck1 = strand.endthickness;
                    double size = thck0 + (thck1 - thck0) * ifac;
                    LVector3 qp1 = pos + oc1 * size;
                    LVector3 qp2 = pos + oc2 * size;
                    LVector3 qp3 = pos + oc3 * size;
                    LVector3 qp4 = pos + oc4 * size;

                    LVector4 col;
                    if (ifac < strand.tcol) {
                        col = col0 + (col1 - col0) * (ifac / strand.tcol);
                    } else {
                        col = col1;
                    }
                    col[3] *= pow(1.0 - ifac, strand.alphaexp);

                    double u0 = 0.0;
                    double v0 = 0.0;
                    double du = 1.0;
                    double dv = 1.0;
                    if (strand.texsplit > 0) {
                        du = dv = 1.0 / strand.texsplit;
                        int frind = int(strand.numframes * ifac);
                        int uind = frind % strand.texsplit;
                        int vind = frind / strand.texsplit;
                        u0 = uind * du;
                        v0 = 1.0 - (vind + 1) * dv;
                    }

                    add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                            qp1, nrm1, col, LVector2(u0, v0),
                            qp2, nrm2, col, LVector2(u0 + du, v0),
                            qp3, nrm3, col, LVector2(u0 + du, v0 + dv));
                    add_tri(gvwvertex, gvwcolor, gvwtexcoord, gvwnormal,
                            qp1, nrm1, col, LVector2(u0, v0),
                            qp3, nrm3, col, LVector2(u0 + du, v0 + dv),
                            qp4, nrm4, col, LVector2(u0, v0 + dv));

                    clear_index += poly_per_part;
                    maxreach = fmax(maxreach, pos.length());
                }
                spart.ctime += adt;
                ++i;
            } else {
                --numparts;
                delete strand.parts[i];
                strand.parts.erase(strand.parts.begin() + i);
            }
        }
        strand.numparts = numparts;

        for(int i = clear_index; i < strand.last_clear_index; ++i) {
            for (int j = 0; j < 3; ++j) {
                gvwvertex->add_data3(0.0, 0.0, 0.0);
            }
        }
        strand.last_clear_index = clear_index;
        if (numparts > 1) {
            LPoint3 center(0.0, 0.0, 0.0);
            PT(BoundingSphere) bnd = new BoundingSphere(center, maxreach);
            strand.node.node()->set_bounds(bnd);
            strand.node.node()->set_final(1);
        }

        delete gvwvertex;
        delete gvwnormal;
        delete gvwtexcoord;
        delete gvwcolor;

        if (!_any_visible && strand.numparts > 0) {
            _any_visible = true;
            _start_point = strand.parts[0]->apos;
            _end_point = strand.parts[strand.numparts - 1]->apos;
        }
    }
}

void PolyBraidGeom::clear (
    const NodePath &camera,
    bool havepq, const LPoint3 &apos, const LQuaternion &aquat)
{
    for (BraidStrandsIter it = _strands.begin(); it != _strands.end(); ++it) {
        BraidStrand &strand = **it;
        GeomVertexRewriter *gvwvertex = new GeomVertexRewriter(strand.gvdata, "vertex");
        strand.gtris->decompose();
        for(int i = 0; i < strand.last_clear_index; ++i) {
            for (int j = 0; j < 3; ++j) {
                gvwvertex->add_data3(0.0, 0.0, 0.0);
            }
        }
        strand.last_clear_index = 0;
        for (StrandParticlesIter it2 = strand.parts.begin(); it2 != strand.parts.end(); ++it2) {
            delete *it2;
        }
        strand.parts.clear();
        strand.numparts = 0;
        delete gvwvertex;
    }

    _any_visible = false;

    if (havepq) {
        _prev_apos = apos;
        _prev_aquat = aquat;
    }
}

bool PolyBraidGeom::any_visible () const
{
    return _any_visible;
}

LPoint3 PolyBraidGeom::prev_apos () const
{
    return _prev_apos;
}

LQuaternion PolyBraidGeom::prev_aquat () const
{
    return _prev_aquat;
}

LPoint3 PolyBraidGeom::start_point () const
{
    return _start_point;
}

LPoint3 PolyBraidGeom::end_point () const
{
    return _end_point;
}

double PolyBraidGeom::seg_period () const
{
    return _segperiod;
}

void PolyBraidGeom::multiply_init_dtang (double spfac)
{
    for (BraidStrandsIter it = _strands.begin(); it != _strands.end(); ++it) {
        BraidStrand &strand = **it;
        strand.dtang = strand.init_dtang * spfac;
    }
}

