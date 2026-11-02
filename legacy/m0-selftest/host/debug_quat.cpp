#include <dolphin/mtx.h>
#include <cstdio>
#include <cmath>

int main() {
    Quaternion q;
    Vec axis = {0.f, 0.f, 1.f};
    C_QUATAxisRad(&q, &axis, 1.5707963f);
    printf("q = %f %f %f %f\n", q.x, q.y, q.z, q.w);

    Quaternion qInv = {-q.x, -q.y, -q.z, q.w};
    Quaternion p = {1.f, 0.f, 0.f, 0.f};  /* (v,0) */

    Quaternion tmp, out;
    C_QUATMultiply(&q, &p, &tmp);
    printf("tmp = %f %f %f %f\n", tmp.x, tmp.y, tmp.z, tmp.w);
    C_QUATMultiply(&tmp, &qInv, &out);
    printf("out = %f %f %f %f\n", out.x, out.y, out.z, out.w);
    return 0;
}
