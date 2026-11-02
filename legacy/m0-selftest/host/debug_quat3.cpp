#include <dolphin/mtx.h>
#include <cstdio>
#include <cmath>

int main() {
    /* Explicit 90-degree quaternion about z */
    Quaternion q = {0.f, 0.f, 0.70710678f, 0.70710678f};
    printf("q = %f %f %f %f\n", q.x, q.y, q.z, q.w);

    Quaternion r = {-q.x, -q.y, -q.z, q.w};
    Quaternion p = {1.f, 0.f, 0.f, 0.f};
    Quaternion t1, t2;
    C_QUATMultiply(&q, &p, &t1);
    C_QUATMultiply(&t1, &r, &t2);
    printf("q p r^-1 = %f %f %f %f\n", t2.x, t2.y, t2.z, t2.w);

    Vec src = {1.f, 0.f, 0.f}, dst;
    C_QUATRotVec(&q, &src, &dst);
    printf("C_QUATRotVec = %f %f %f\n", dst.x, dst.y, dst.z);

    Mtx m;
    C_MTXQuat(m, &q);
    C_MTXMultVec(m, &src, &dst);
    printf("C_MTXQuat mtx = %f %f %f\n", dst.x, dst.y, dst.z);
    return 0;
}
