// SPDX-License-Identifier: Apache-2.0
// Real EG25 NMEA transport via ModemManager; no mock/synthetic location provider.
#define LOG_TAG "PinePhoneGnss"
#include <hardware/gps.h>
#include <log/log.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex mutex;
std::mutex lifecycle;
std::condition_variable wake;
GpsCallbacks callbacks{};
std::thread worker;
std::atomic<bool> running{false};
uint32_t interval_ms = 1000;
GpsPositionRecurrence recurrence = GPS_POSITION_RECURRENCE_PERIODIC;

bool command(const char* action, std::string* output = nullptr) {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) return false;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) dup2(nullfd, STDERR_FILENO);
        close(pipes[0]); close(pipes[1]);
        execl("/vendor/bin/mmcli", "mmcli", "--timeout=10", "--modem=any", action,
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return false; }
    std::string text;
    char buffer[4096];
    ssize_t size;
    while ((size = read(pipes[0], buffer, sizeof(buffer))) > 0) {
        if (text.size() + size <= 512 * 1024) text.append(buffer, size);
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    if (output) *output = std::move(text);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

GpsCallbacks snapshot() { std::lock_guard<std::mutex> guard(mutex); return callbacks; }
void status(GpsStatusValue value) {
    auto cb = snapshot();
    if (cb.status_cb) { GpsStatus result{sizeof(GpsStatus), value}; cb.status_cb(&result); }
}
std::vector<std::string> split(const std::string& input) {
    std::vector<std::string> result;
    size_t start = 0;
    for (;;) {
        size_t comma = input.find(',', start);
        result.push_back(input.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}
bool satellite_id(const std::string& talker, int prn, GnssConstellationType* constellation, int16_t* svid) {
    if (prn <= 0) return false;
    if (talker == "GL" || (talker == "GN" && prn >= 65 && prn <= 96)) {
        *constellation = GNSS_CONSTELLATION_GLONASS;
        *svid = prn >= 65 ? prn - 64 : prn;
    } else if (talker == "GB" || talker == "BD" || (talker == "GN" && prn >= 201 && prn <= 263)) {
        *constellation = GNSS_CONSTELLATION_BEIDOU;
        *svid = prn >= 201 ? prn - 200 : prn;
    } else if (talker == "GA" || (talker == "GN" && prn >= 301 && prn <= 336)) {
        *constellation = GNSS_CONSTELLATION_GALILEO;
        *svid = prn >= 301 ? prn - 300 : prn;
    } else if (prn >= 193 && prn <= 200) {
        *constellation = GNSS_CONSTELLATION_QZSS;
        *svid = prn - 192;
    } else if (prn >= 33 && prn <= 64) {
        *constellation = GNSS_CONSTELLATION_SBAS;
        *svid = prn + 87;
    } else if ((talker == "GP" || talker == "GN") && prn <= 32) {
        *constellation = GNSS_CONSTELLATION_GPS;
        *svid = prn;
    } else return false;
    return true;
}
bool number(const std::string& input, double* value) {
    if (input.empty()) return false;
    char* end = nullptr;
    *value = strtod(input.c_str(), &end);
    return end && *end == 0 && std::isfinite(*value);
}
bool coordinate(const std::string& input, const std::string& hemisphere, double* result, bool latitude) {
    double raw;
    if (!number(input, &raw) || raw < 0) return false;
    double degrees = std::floor(raw / 100), minutes = raw - degrees * 100;
    if (minutes >= 60 || degrees > (latitude ? 90 : 180)) return false;
    if (hemisphere != (latitude ? "N" : "E") && hemisphere != (latitude ? "S" : "W")) return false;
    *result = degrees + minutes / 60;
    if (*result > (latitude ? 90 : 180)) return false;
    if (hemisphere == "S" || hemisphere == "W") *result = -*result;
    return true;
}
bool timestamp(const std::string& utc, const std::string& date, GpsUtcTime* result) {
    if (utc.size() < 6 || date.size() != 6) return false;
    for (size_t i = 0; i < 6; ++i) {
        if (utc[i] < '0' || utc[i] > '9' || date[i] < '0' || date[i] > '9') return false;
    }
    struct tm time{};
    time.tm_mday = std::stoi(date.substr(0, 2));
    time.tm_mon = std::stoi(date.substr(2, 2)) - 1;
    int year = std::stoi(date.substr(4, 2));
    time.tm_year = (year >= 80 ? 1900 : 2000) + year - 1900;
    time.tm_hour = std::stoi(utc.substr(0, 2));
    time.tm_min = std::stoi(utc.substr(2, 2));
    double seconds;
    if (!number(utc.substr(4), &seconds) || seconds < 0 || seconds >= 61 || time.tm_hour > 23 || time.tm_min > 59 || time.tm_mon < 0 || time.tm_mon > 11 || time.tm_mday < 1 || time.tm_mday > 31) return false;
    time.tm_sec = static_cast<int>(seconds);
    int day = time.tm_mday, month = time.tm_mon, full_year = time.tm_year;
    time_t epoch = timegm(&time);
    if (epoch < 0) return false;
    if (time.tm_mday != day || time.tm_mon != month || time.tm_year != full_year) return false;
    *result = static_cast<GpsUtcTime>(epoch) * 1000 + static_cast<int>((seconds - std::floor(seconds)) * 1000);
    return true;
}
bool sentence(const std::string& line, std::string* body, std::string* full) {
    size_t start = line.find('$'), end = line.find('*', start);
    if (start == std::string::npos || end == std::string::npos || end + 2 >= line.size()) return false;
    unsigned checksum = 0;
    for (size_t i = start + 1; i < end; ++i) checksum ^= static_cast<unsigned char>(line[i]);
    std::string check = line.substr(end + 1, 2);
    char* tail = nullptr;
    unsigned expected = strtoul(check.c_str(), &tail, 16);
    if (!tail || *tail != 0 || expected != checksum) return false;
    *body = line.substr(start + 1, end - start - 1);
    *full = line.substr(start, end - start + 3);
    return true;
}

void run() {
    if (!command("--location-enable-gps-nmea")) {
        ALOGE("Could not enable the modem's real NMEA source");
        running = false;
        status(GPS_STATUS_ENGINE_OFF);
        return;
    }
    status(GPS_STATUS_ENGINE_ON);
    command("--location-set-gps-refresh-rate=1");
    status(GPS_STATUS_SESSION_BEGIN);
    GpsUtcTime last_fix = 0;
    std::string last_output;
    while (running) {
        std::string output;
        if (command("--location-get", &output) && output != last_output) {
            last_output = output;
            GpsLocation fix{};
            fix.size = sizeof(fix);
            GnssSvStatus satellites{};
            satellites.size = sizeof(satellites);
            bool valid = false;
            bool receiver_accuracy = false;
            size_t pos = 0;
            while (pos < output.size()) {
                size_t end = output.find('\n', pos);
                std::string line = output.substr(pos, end == std::string::npos ? end : end - pos);
                pos = end == std::string::npos ? output.size() : end + 1;
                std::string body, full;
                if (!sentence(line, &body, &full)) continue;
                auto fields = split(body);
                if (fields.empty() || fields[0].size() < 3) continue;
                std::string type = fields[0].substr(fields[0].size() - 3);
                auto cb = snapshot();
                if (cb.nmea_cb) cb.nmea_cb(static_cast<GpsUtcTime>(::time(nullptr)) * 1000, full.c_str(), full.size());
                if (type == "RMC" && fields.size() > 9 && fields[2] == "A") {
                    valid = coordinate(fields[3], fields[4], &fix.latitude, true) &&
                            coordinate(fields[5], fields[6], &fix.longitude, false) &&
                            timestamp(fields[1], fields[9], &fix.timestamp);
                    if (valid) {
                        fix.flags |= GPS_LOCATION_HAS_LAT_LONG;
                        double value;
                        if (number(fields[7], &value) && value >= 0) { fix.speed = value * 0.514444444; fix.flags |= GPS_LOCATION_HAS_SPEED; }
                        if (number(fields[8], &value) && value >= 0 && value < 360) { fix.bearing = value; fix.flags |= GPS_LOCATION_HAS_BEARING; }
                    }
                } else if (type == "GGA" && fields.size() > 11 && fields[6] != "0") {
                    double altitude, separation, hdop;
                    if (number(fields[9], &altitude) && number(fields[11], &separation)) { fix.altitude = altitude + separation; fix.flags |= GPS_LOCATION_HAS_ALTITUDE; }
                    // Receiver HDOP converted to an explicit nominal UERE estimate;
                    // GST below supersedes this when receiver uncertainty is available.
                    if (!receiver_accuracy && number(fields[8], &hdop) && hdop > 0) { fix.accuracy = hdop * 5; fix.flags |= GPS_LOCATION_HAS_ACCURACY; }
                } else if (type == "GST" && fields.size() > 7) {
                    double latitude_sigma, longitude_sigma;
                    if (number(fields[6], &latitude_sigma) && number(fields[7], &longitude_sigma) && latitude_sigma >= 0 && longitude_sigma >= 0) {
                        fix.accuracy = std::hypot(latitude_sigma, longitude_sigma);
                        fix.flags |= GPS_LOCATION_HAS_ACCURACY;
                        receiver_accuracy = true;
                    }
                } else if (type == "GSV" && fields.size() >= 8) {
                    std::string talker = fields[0].substr(0, 2);
                    for (size_t i = 4; i + 3 < fields.size() && satellites.num_svs < GNSS_MAX_SVS; i += 4) {
                        double prn, elevation, azimuth, strength = 0;
                        if (!number(fields[i], &prn) || std::floor(prn) != prn ||
                            !number(fields[i + 1], &elevation) || elevation < 0 || elevation > 90 ||
                            !number(fields[i + 2], &azimuth) || azimuth < 0 || azimuth >= 360) continue;
                        GnssSvInfo info{};
                        info.size = sizeof(info);
                        if (!satellite_id(talker, static_cast<int>(prn), &info.constellation, &info.svid)) continue;
                        if (!fields[i + 3].empty() && (!number(fields[i + 3], &strength) || strength < 0)) continue;
                        info.c_n0_dbhz = strength;
                        info.elevation = elevation;
                        info.azimuth = azimuth;
                        satellites.gnss_sv_list[satellites.num_svs++] = info;
                    }
                }
            }
            if (satellites.num_svs) {
                auto cb = snapshot();
                if (cb.gnss_sv_status_cb) cb.gnss_sv_status_cb(&satellites);
            }
            if (valid && fix.timestamp > last_fix) {
                last_fix = fix.timestamp;
                auto cb = snapshot();
                if (cb.location_cb) cb.location_cb(&fix);
                std::lock_guard<std::mutex> guard(mutex);
                if (recurrence == GPS_POSITION_RECURRENCE_SINGLE) running = false;
            }
        }
        std::unique_lock<std::mutex> guard(mutex);
        wake.wait_for(guard, std::chrono::milliseconds(interval_ms), [] { return !running.load(); });
    }
    command("--location-disable-gps-nmea");
    status(GPS_STATUS_SESSION_END);
    status(GPS_STATUS_ENGINE_OFF);
}

int init(GpsCallbacks* cb) {
    if (!cb || cb->size < sizeof(GpsCallbacks)) return -EINVAL;
    { std::lock_guard<std::mutex> guard(mutex); callbacks = *cb; }
    if (cb->set_capabilities_cb) cb->set_capabilities_cb(GPS_CAPABILITY_SCHEDULING);
    return 0;
}
int start() {
    std::lock_guard<std::mutex> guard(lifecycle);
    if (running) return 0;
    if (worker.joinable()) worker.join();
    running = true;
    worker = std::thread(run);
    return 0;
}
int stop() {
    std::lock_guard<std::mutex> guard(lifecycle);
    running = false;
    wake.notify_all();
    if (worker.joinable()) worker.join();
    return 0;
}
void cleanup() { stop(); std::lock_guard<std::mutex> guard(mutex); callbacks = {}; }
int inject_time(GpsUtcTime, int64_t, int) { return 0; }
int inject_location(double, double, float) { return 0; }
void delete_aiding_data(GpsAidingData) {}
int position_mode(GpsPositionMode mode, GpsPositionRecurrence repeat, uint32_t minimum, uint32_t, uint32_t) {
    if (mode != GPS_POSITION_MODE_STANDALONE) return -ENOTSUP;
    std::lock_guard<std::mutex> guard(mutex);
    interval_ms = std::max(1000u, minimum);
    recurrence = repeat;
    return 0;
}
const void* extension(const char*) { return nullptr; }
const GpsInterface interface = {sizeof(GpsInterface), init, start, stop, cleanup, inject_time,
                               inject_location, delete_aiding_data, position_mode, extension};
const GpsInterface* get_interface(gps_device_t*) { return &interface; }
int device_close(hw_device_t* device) { cleanup(); delete reinterpret_cast<gps_device_t*>(device); return 0; }
int device_open(const hw_module_t* module, const char*, hw_device_t** device) {
    auto gps = new gps_device_t{};
    gps->common.tag = HARDWARE_DEVICE_TAG;
    gps->common.module = const_cast<hw_module_t*>(module);
    gps->common.close = device_close;
    gps->get_gps_interface = get_interface;
    *device = &gps->common;
    return 0;
}
hw_module_methods_t methods = {device_open};
}

extern "C" {
hw_module_t HAL_MODULE_INFO_SYM = {
    HARDWARE_MODULE_TAG, HARDWARE_MODULE_API_VERSION(1, 0), HARDWARE_HAL_API_VERSION,
    GPS_HARDWARE_MODULE_ID, "PinePhone Pro EG25 real NMEA GNSS", "PinePhone Pro project",
    &methods, nullptr, {0}
};
}
