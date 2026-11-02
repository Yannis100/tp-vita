#include <dolphin/mtx.h>
#include <cstdio>
#include <cmath>

int main() {
    Quaternion qa, qb, qr;
    Vec zAxis = {0.f, 0.f, 1.f};
    C_QUATAxisRad(&qa, &zAxis, 0.785398f);  /* 45deg */
    C_QUATAxisRad(&qb, &zAxis, 0.785398f);  /* 45deg */
    printf("qa = %f %f %f %f\n", qa.x, qa.y, qa.z, qa.w);
    printf("qb = %f %f %f %f\n", qb.x, qb.y, qb.z, qb.w);
    C_QUATMultiply(&qa, &qb, &qr);
    printf("qr = qa*qb = %f %f %f %f\n", qr.x, qr.y, qr.z, qr.w);

    Vec src = {1.f, 0.f, 0.f}, dst;
    C_QUATRotVec(&qr, &src, &dst);
    printf("rot(1,0,0) = %f %f %f\n", dst.x, dst.y, dst.z);
    return 0;
}
