// Spike S1: shared test scene. 20 turned-part-like bodies, 10 faces each, exactly 500,000 triangles,
// 50,000 edge segments. Pure C++, no Qt, no OCCT, so A and B draw identical geometry.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace s1 {

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    std::uint8_t id[4];   // face id, little endian; 0 is background
};
static_assert(sizeof(Vertex) == 28);

struct Body {
    std::uint32_t firstIndex, indexCount;
    std::uint32_t firstVertex, vertexCount;
    std::uint32_t firstEdgeVertex, edgeVertexCount;
};

struct Mesh {
    std::vector<Vertex> verts;
    std::vector<std::uint32_t> indices;       // absolute vertex indices
    std::vector<float> edgeVerts;             // xyz pairs, line list
    std::vector<Body> bodies;
    std::vector<std::array<float, 3>> probe;  // probe[faceId-1] = a point on that face
    std::uint32_t triangleCount() const { return std::uint32_t(indices.size() / 3); }
};

constexpr int kBodiesX = 5, kBodiesY = 4, kFaces = 10, kRows = 5, kSegments = 250;
constexpr float kPi = 3.14159265358979f;
constexpr int kTopFace = 8;   // index of the upward-facing top annulus in the profile

inline std::uint32_t faceId(int body, int face) { return std::uint32_t(body * kFaces + face + 1); }

inline Mesh buildMesh()
{
    // Closed profile (r, z), counter-clockwise, so the outward normal of a segment d is (dz, -dr).
    static const float prof[kFaces][2] = {
        {10, 0}, {40, 0}, {40, 12}, {30, 12}, {30, 30}, {22, 40}, {22, 50}, {16, 50}, {16, 60}, {10, 60}};
    Mesh m;
    const int nBodies = kBodiesX * kBodiesY;
    m.verts.reserve(std::size_t(nBodies) * kFaces * (kRows + 1) * (kSegments + 1));
    m.indices.reserve(std::size_t(nBodies) * kFaces * kRows * kSegments * 6);
    m.edgeVerts.reserve(std::size_t(nBodies) * kFaces * kSegments * 6);
    m.probe.resize(std::size_t(nBodies) * kFaces);
    for (int b = 0; b < nBodies; ++b) {
        const int ix = b % kBodiesX, iy = b / kBodiesX;
        const float ox = (ix - (kBodiesX - 1) * 0.5f) * 110.0f, oy = (iy - (kBodiesY - 1) * 0.5f) * 110.0f;
        const float s = 0.85f + 0.3f * float((b * 7919) % 13) / 12.0f;
        Body body{};
        body.firstIndex = std::uint32_t(m.indices.size());
        body.firstVertex = std::uint32_t(m.verts.size());
        body.firstEdgeVertex = std::uint32_t(m.edgeVerts.size() / 3);
        for (int f = 0; f < kFaces; ++f) {
            const float r0 = prof[f][0] * s, z0 = prof[f][1] * s;
            const float r1 = prof[(f + 1) % kFaces][0] * s, z1 = prof[(f + 1) % kFaces][1] * s;
            float nr = z1 - z0, nz = -(r1 - r0);
            const float nl = std::sqrt(nr * nr + nz * nz);
            nr /= nl; nz /= nl;
            const std::uint32_t id = faceId(b, f);
            const std::uint32_t base = std::uint32_t(m.verts.size());
            for (int i = 0; i <= kRows; ++i) {
                const float t = float(i) / kRows;
                const float r = r0 + (r1 - r0) * t, z = z0 + (z1 - z0) * t;
                for (int j = 0; j <= kSegments; ++j) {
                    const float a = 2 * kPi * float(j % kSegments) / kSegments;
                    const float c = std::cos(a), sn = std::sin(a);
                    Vertex v{ox + r * c, oy + r * sn, z, nr * c, nr * sn, nz,
                             {std::uint8_t(id), std::uint8_t(id >> 8), std::uint8_t(id >> 16), std::uint8_t(id >> 24)}};
                    m.verts.push_back(v);
                }
            }
            const std::uint32_t w = kSegments + 1;
            for (int i = 0; i < kRows; ++i)
                for (int j = 0; j < kSegments; ++j) {
                    const std::uint32_t p00 = base + i * w + j, p01 = p00 + 1, p10 = p00 + w, p11 = p10 + 1;
                    m.indices.insert(m.indices.end(), {p00, p01, p10, p10, p01, p11});
                }
            // feature edge: the circle at the start of this profile segment
            for (int j = 0; j < kSegments; ++j) {
                const float a0 = 2 * kPi * float(j) / kSegments, a1 = 2 * kPi * float((j + 1) % kSegments) / kSegments;
                m.edgeVerts.insert(m.edgeVerts.end(), {ox + r0 * std::cos(a0), oy + r0 * std::sin(a0), z0,
                                                       ox + r0 * std::cos(a1), oy + r0 * std::sin(a1), z0});
            }
            const float pr = (r0 + r1) * 0.5f, pz = (z0 + z1) * 0.5f, pa = 0.3f;
            m.probe[id - 1] = {ox + pr * std::cos(pa), oy + pr * std::sin(pa), pz};
        }
        body.indexCount = std::uint32_t(m.indices.size()) - body.firstIndex;
        body.vertexCount = std::uint32_t(m.verts.size()) - body.firstVertex;
        body.edgeVertexCount = std::uint32_t(m.edgeVerts.size() / 3) - body.firstEdgeVertex;
        m.bodies.push_back(body);
    }
    return m;
}

// Orbit camera shared by A and B. Z is up. One revolution per `period` seconds.
struct Cam { float eye[3], target[3]; float fovYDeg, zNear, zFar; };
inline Cam orbitCamera(double seconds, double period = 10.0)
{
    const double az = 2.0 * 3.14159265358979 * seconds / period, el = 35.0 * 3.14159265358979 / 180.0, d = 700.0;
    Cam c{};
    c.target[0] = 0; c.target[1] = 0; c.target[2] = 30;
    c.eye[0] = float(d * std::cos(el) * std::cos(az));
    c.eye[1] = float(d * std::cos(el) * std::sin(az));
    c.eye[2] = float(30 + d * std::sin(el));
    c.fovYDeg = 35; c.zNear = 50; c.zFar = 2000;
    return c;
}

} // namespace s1
