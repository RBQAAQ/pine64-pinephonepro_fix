// SPDX-License-Identifier: Apache-2.0
#include <android/hardware/gnss/1.0/IGnss.h>
#include <hidl/LegacySupport.h>

int main() {
    return android::hardware::defaultPassthroughServiceImplementation<
        android::hardware::gnss::V1_0::IGnss>();
}
