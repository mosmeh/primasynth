#include "stereovalue.h"

namespace primasynth {

StereoValue StereoValue::operator+(const StereoValue& b) const {
    return {left + b.left, right + b.right};
}

StereoValue StereoValue::operator*(double b) const {
    return {left * b, right * b};
}

StereoValue StereoValue::operator*(const StereoValue& b) const {
    return {left * b.left, right * b.right};
}

StereoValue& StereoValue::operator+=(const StereoValue& b) {
    left += b.left;
    right += b.right;
    return *this;
}

StereoValue operator*(double a, const StereoValue& b) {
    return {a * b.left, a * b.right};
}

}
