// Minimal linear algebra in USD's conventions: row vectors (p' = p * M), row-major storage,
// matrices compose left to right (A * B applies A first).
#pragma once

#include <cmath>
#include <cstring>

namespace scene {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double Length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 Normalized() const {
        double l = Length();
        return l > 0 ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
    }
};

struct Mat4 {
    double m[16];

    static Mat4 Identity() {
        Mat4 r;
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
        return r;
    }
    static Mat4 FromArray(const double* a) {
        Mat4 r;
        std::memcpy(r.m, a, sizeof(r.m));
        return r;
    }
    double& operator()(int r, int c) { return m[r * 4 + c]; }
    double operator()(int r, int c) const { return m[r * 4 + c]; }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i * 4 + j] = m[i * 4 + 0] * b.m[0 * 4 + j] + m[i * 4 + 1] * b.m[1 * 4 + j] +
                                 m[i * 4 + 2] * b.m[2 * 4 + j] + m[i * 4 + 3] * b.m[3 * 4 + j];
        return r;
    }

    Vec3 TransformPoint(const Vec3& p) const {
        double x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
        double y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
        double z = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
        double w = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
        if (w != 0.0 && w != 1.0) return {x / w, y / w, z / w};
        return {x, y, z};
    }
    Vec3 TransformDir(const Vec3& d) const {
        return {d.x * m[0] + d.y * m[4] + d.z * m[8], d.x * m[1] + d.y * m[5] + d.z * m[9],
                d.x * m[2] + d.y * m[6] + d.z * m[10]};
    }

    Mat4 Transposed() const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) r.m[i * 4 + j] = m[j * 4 + i];
        return r;
    }

    Mat4 Inverse() const {
        const double* a = m;
        double inv[16];
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (std::fabs(det) < 1e-300) return Identity();
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
        return r;
    }

    static Mat4 Translate(double x, double y, double z) {
        Mat4 r = Identity();
        r.m[12] = x;
        r.m[13] = y;
        r.m[14] = z;
        return r;
    }
    static Mat4 Scale(double x, double y, double z) {
        Mat4 r = Identity();
        r.m[0] = x;
        r.m[5] = y;
        r.m[10] = z;
        return r;
    }
    static Mat4 RotateAxis(int axis, double degrees) {
        double a = degrees * 3.14159265358979323846 / 180.0;
        double c = std::cos(a), s = std::sin(a);
        Mat4 r = Identity();
        if (axis == 0) {
            r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c;
        } else if (axis == 1) {
            r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
        } else {
            r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        }
        return r;
    }
    // Rotation for a unit quaternion (x, y, z, w) in row-vector convention.
    static Mat4 FromQuat(double x, double y, double z, double w) {
        double len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len > 0) {
            x /= len; y /= len; z /= len; w /= len;
        } else {
            w = 1;
        }
        Mat4 r = Identity();
        r.m[0] = 1 - 2 * (y * y + z * z);
        r.m[1] = 2 * (x * y + w * z);
        r.m[2] = 2 * (x * z - w * y);
        r.m[4] = 2 * (x * y - w * z);
        r.m[5] = 1 - 2 * (x * x + z * z);
        r.m[6] = 2 * (y * z + w * x);
        r.m[8] = 2 * (x * z + w * y);
        r.m[9] = 2 * (y * z - w * x);
        r.m[10] = 1 - 2 * (x * x + y * y);
        return r;
    }
};

// Translation, rotation (unit quaternion x, y, z, w) and scale: the form transforms are blended in.
// Composes as Scale * Rotate * Translate (row vectors), like UsdSkel joint transforms.
struct TRS {
    double t[3] = {0, 0, 0};
    double q[4] = {0, 0, 0, 1};
    double s[3] = {1, 1, 1};

    Mat4 ToMatrix() const { return Mat4::Scale(s[0], s[1], s[2]) * Mat4::FromQuat(q[0], q[1], q[2], q[3]) * Mat4::Translate(t[0], t[1], t[2]); }

    // Assumes no shear; a mirrored matrix gets a negative x scale.
    static TRS FromMatrix(const Mat4& m) {
        TRS r;
        const double* a = m.m;
        r.t[0] = a[12];
        r.t[1] = a[13];
        r.t[2] = a[14];
        double rows[3][3];
        for (int i = 0; i < 3; ++i) {
            r.s[i] = std::sqrt(a[i * 4] * a[i * 4] + a[i * 4 + 1] * a[i * 4 + 1] + a[i * 4 + 2] * a[i * 4 + 2]);
            for (int k = 0; k < 3; ++k) rows[i][k] = r.s[i] > 0 ? a[i * 4 + k] / r.s[i] : (i == k ? 1.0 : 0.0);
        }
        const double det = rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1]) -
                           rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0]) +
                           rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]);
        if (det < 0) {
            r.s[0] = -r.s[0];
            for (int k = 0; k < 3; ++k) rows[0][k] = -rows[0][k];
        }
        // Rotation matrix (row-vector convention) to quaternion; inverse of Mat4::FromQuat.
        const double m0 = rows[0][0], m1 = rows[0][1], m2 = rows[0][2], m4 = rows[1][0], m5 = rows[1][1], m6 = rows[1][2],
                     m8 = rows[2][0], m9 = rows[2][1], m10 = rows[2][2];
        const double trace = m0 + m5 + m10;
        double x, y, z, w;
        if (trace > 0) {
            const double k = std::sqrt(trace + 1.0) * 2;
            w = k / 4; x = (m6 - m9) / k; y = (m8 - m2) / k; z = (m1 - m4) / k;
        } else if (m0 > m5 && m0 > m10) {
            const double k = std::sqrt(1.0 + m0 - m5 - m10) * 2;
            x = k / 4; w = (m6 - m9) / k; y = (m1 + m4) / k; z = (m8 + m2) / k;
        } else if (m5 > m10) {
            const double k = std::sqrt(1.0 + m5 - m0 - m10) * 2;
            y = k / 4; w = (m8 - m2) / k; x = (m1 + m4) / k; z = (m6 + m9) / k;
        } else {
            const double k = std::sqrt(1.0 + m10 - m0 - m5) * 2;
            z = k / 4; w = (m1 - m4) / k; x = (m8 + m2) / k; y = (m6 + m9) / k;
        }
        const double len = std::sqrt(x * x + y * y + z * z + w * w);
        r.q[0] = x / len; r.q[1] = y / len; r.q[2] = z / len; r.q[3] = w / len;
        return r;
    }

    // Lerp translation and scale, shortest-path slerp rotation.
    static TRS Blend(const TRS& a, const TRS& b, double u) {
        TRS r;
        for (int i = 0; i < 3; ++i) {
            r.t[i] = a.t[i] + (b.t[i] - a.t[i]) * u;
            r.s[i] = a.s[i] + (b.s[i] - a.s[i]) * u;
        }
        double qb[4] = {b.q[0], b.q[1], b.q[2], b.q[3]};
        double d = a.q[0] * qb[0] + a.q[1] * qb[1] + a.q[2] * qb[2] + a.q[3] * qb[3];
        if (d < 0) {
            d = -d;
            for (double& c : qb) c = -c;
        }
        double s0 = 1 - u, s1 = u;
        if (d < 0.9995) {
            const double theta = std::acos(d > 1 ? 1 : d), st = std::sin(theta);
            s0 = std::sin((1 - u) * theta) / st;
            s1 = std::sin(u * theta) / st;
        }
        double len = 0;
        for (int i = 0; i < 4; ++i) {
            r.q[i] = s0 * a.q[i] + s1 * qb[i];
            len += r.q[i] * r.q[i];
        }
        len = std::sqrt(len);
        for (double& c : r.q) c = len > 0 ? c / len : c;
        return r;
    }
};

struct Bounds {
    Vec3 min{1e300, 1e300, 1e300};
    Vec3 max{-1e300, -1e300, -1e300};
    bool IsEmpty() const { return min.x > max.x; }
    void Add(const Vec3& p) {
        if (p.x < min.x) min.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.z < min.z) min.z = p.z;
        if (p.x > max.x) max.x = p.x;
        if (p.y > max.y) max.y = p.y;
        if (p.z > max.z) max.z = p.z;
    }
    void Add(const Bounds& b) {
        if (!b.IsEmpty()) {
            Add(b.min);
            Add(b.max);
        }
    }
    Vec3 Center() const { return (min + max) * 0.5; }
    double Radius() const { return (max - min).Length() * 0.5; }
};

}  // namespace scene
