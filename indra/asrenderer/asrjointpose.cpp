/**
 * asrjointpose.cpp
 * Extract rotation once per incoming pose, rather than per skin or render frame.
 * Author: chanayane@firestorm
 */
#include "asrjointpose.h"

#include <algorithm>
#include <cmath>

namespace asr
{
namespace
{

using Matrix = double[3][3];

bool inverse(const Matrix& m, Matrix& out)
{
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-20)
    {
        return false;
    }
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            out[column][row] = (m[(row + 1) % 3][(column + 1) % 3] * m[(row + 2) % 3][(column + 2) % 3] -
                                m[(row + 1) % 3][(column + 2) % 3] * m[(row + 2) % 3][(column + 1) % 3]) / determinant;
        }
    }
    return true;
}

} // namespace

void jointRotationDelta(const float newest[3][4], const float previous[3][4], float quaternion[4])
{
    quaternion[0] = quaternion[1] = quaternion[2] = 0.f;
    quaternion[3] = 1.f;
    Matrix before{}, inverted{}, rotation{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            before[row][column] = previous[row][column];
        }
    }
    if (!inverse(before, inverted))
    {
        return;
    }
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            for (int k = 0; k < 3; ++k)
            {
                rotation[row][column] += double(newest[row][k]) * inverted[k][column];
            }
        }
    }

    // Scaled Newton polar decomposition removes any change in scale/shear. With an
    // unchanged affine shape, newest * inverse(previous) is already the exact rotation.
    for (int iteration = 0; iteration < 12; ++iteration)
    {
        if (!inverse(rotation, inverted))
        {
            return;
        }
        double norm = 0.0, inverseNorm = 0.0;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                norm += rotation[row][column] * rotation[row][column];
                inverseNorm += inverted[row][column] * inverted[row][column];
            }
        }
        const double balance = std::sqrt(std::sqrt(inverseNorm / norm));
        if (!std::isfinite(balance) || balance == 0.0)
        {
            return;
        }
        double change = 0.0;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const double next = 0.5 * (balance * rotation[row][column] + inverted[column][row] / balance);
                change = std::max(change, std::fabs(next - rotation[row][column]));
                rotation[row][column] = next;
            }
        }
        if (change < 1e-10)
        {
            break;
        }
        if (iteration == 11)
        {
            return;
        }
    }
    const double determinant = rotation[0][0] * (rotation[1][1] * rotation[2][2] - rotation[1][2] * rotation[2][1]) -
                               rotation[0][1] * (rotation[1][0] * rotation[2][2] - rotation[1][2] * rotation[2][0]) +
                               rotation[0][2] * (rotation[1][0] * rotation[2][1] - rotation[1][1] * rotation[2][0]);
    if (!(determinant > 0.0))
    {
        return;
    }
    double q[4]{};
    const double trace = rotation[0][0] + rotation[1][1] + rotation[2][2];
    if (trace > 0.0)
    {
        const double size = 2.0 * std::sqrt(trace + 1.0);
        q[3] = size * 0.25;
        q[0] = (rotation[2][1] - rotation[1][2]) / size;
        q[1] = (rotation[0][2] - rotation[2][0]) / size;
        q[2] = (rotation[1][0] - rotation[0][1]) / size;
    }
    else
    {
        int i = rotation[1][1] > rotation[0][0] ? 1 : 0;
        if (rotation[2][2] > rotation[i][i]) i = 2;
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        const double size = 2.0 * std::sqrt(1.0 + rotation[i][i] - rotation[j][j] - rotation[k][k]);
        q[i] = size * 0.25;
        q[j] = (rotation[j][i] + rotation[i][j]) / size;
        q[k] = (rotation[k][i] + rotation[i][k]) / size;
        q[3] = (rotation[k][j] - rotation[j][k]) / size;
    }
    const double length = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (!std::isfinite(length) || length == 0.0) return;
    const double factor = (q[3] < 0.0 ? -1.0 : 1.0) / length;
    for (int i = 0; i < 4; ++i) quaternion[i] = float(q[i] * factor);
}

void extrapolateJointRows(const float newest[3][4], const float previous[3][4],
                          const float quaternion[4], float lead, float result[3][4])
{
    const float sine = std::sqrt(quaternion[0]*quaternion[0] + quaternion[1]*quaternion[1] + quaternion[2]*quaternion[2]);
    const float angle = lead * std::atan2(sine, quaternion[3]);
    const float factor = sine > 1e-6f ? std::sin(angle) / sine : lead;
    const float x = quaternion[0]*factor, y = quaternion[1]*factor, z = quaternion[2]*factor, w = std::cos(angle);
    for (int column = 0; column < 3; ++column)
    {
        const float a = newest[0][column], b = newest[1][column], c = newest[2][column];
        const float tx = 2.f * (y*c - z*b), ty = 2.f * (z*a - x*c), tz = 2.f * (x*b - y*a);
        result[0][column] = a + w*tx + y*tz - z*ty;
        result[1][column] = b + w*ty + z*tx - x*tz;
        result[2][column] = c + w*tz + x*ty - y*tx;
    }
    for (int row = 0; row < 3; ++row)
    {
        result[row][3] = newest[row][3] + (newest[row][3] - previous[row][3]) * lead;
    }
}

} // namespace asr
