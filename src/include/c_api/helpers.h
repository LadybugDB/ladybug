#pragma once

#include <cstdint>
#include <exception>
#include <string>
#ifdef _WIN32
#include <time.h>

#include <windows.h>

time_t convertTmToTime(struct tm tm);

int32_t convertTimeToTm(time_t time, struct tm* out_tm);
#endif

void setLastCAPIErrorMessage(const std::string& message);

void clearLastCAPIErrorMessage();

char* takeLastCAPIErrorMessage();

char* convertToOwnedCString(const std::string& str);

// Exception floor for C API entry points. No C++ exception may cross the C
// boundary: an escaping exception in an extern "C" function reaches
// std::terminate in the caller's process (observed from embedding runtimes).
// Wrap the throwing region of an entry point:
//   LBUG_C_API_GUARD_BEGIN
//   ... body ...
//   LBUG_C_API_GUARD_END(<value to return on error>)
#define LBUG_C_API_GUARD_BEGIN try {
#define LBUG_C_API_GUARD_END(err_ret)                                                              \
    }                                                                                              \
    catch (const std::exception& e) {                                                              \
        setLastCAPIErrorMessage(e.what());                                                         \
        return err_ret;                                                                            \
    }                                                                                              \
    catch (...) {                                                                                  \
        setLastCAPIErrorMessage("unknown C++ exception in lbug C API");                            \
        return err_ret;                                                                            \
    }

// Void-returning variant of the exception floor: swallows the exception after
// recording it, since there is no error value to return.
#define LBUG_C_API_GUARD_END_VOID                                                                  \
    }                                                                                              \
    catch (const std::exception& e) {                                                              \
        setLastCAPIErrorMessage(e.what());                                                         \
    }                                                                                              \
    catch (...) {                                                                                  \
        setLastCAPIErrorMessage("unknown C++ exception in lbug C API");                            \
    }
