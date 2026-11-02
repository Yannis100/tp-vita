/* debug_mtx.cpp - numeric check of C_MTXInverse and C_QUATRotVec */
#include <dolphin/mtx.h>
#include <cstdio>
#include <cmath>

int main() {
    Mtx m, inv, prod;
    C_MTXIdentity(m);
    /* rotation about z by 30deg + translation */
    m[0][0] = 0.866025f; m[0][1] = -0.5f; m[0][3] = 1.0f;
    m[1][0] = 0.5f;     m[1][1] = 0.866025f; m[1][3] = 2.0f;
    m[2][2] = 1.0f; m[2][3] = 3.0f;

    printf("det check inverse...\n");
    if (!C_MTXInverse(m, inv)) {
        printf("not invertible!\n");
        return 1;
    }
    C_MTXConcat(m, inv, prod);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            printf("%+.4f ", prod[i][j]);
        }
        printf("\n");
    }

    printf("\nquat rot check:\n");
    Quaternion q;
    Vec axis = {0.f, 0.f, 1.f};
    C_QUATAxisRad(&q, &axis, 1.5707963f); /* 90deg */
    printf("q = %f %f %f %f\n", q.x, q.y, q.z, q.w);
    Vec src = {1.f, 0.f, 0.f}, dst;
    C_QUATRotVec(&q, &src, &dst);
    printf("rot(1,0,0) = %f %f %f\n", dst.x, dst.y, dst.z);
    return 0;
}
