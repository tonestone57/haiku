#ifndef THREAD_DATA_H
#define THREAD_DATA_H

#include <support/SupportDefs.h>

class ThreadData {
public:
    // Assuming ThreadData needs to store a virtual deadline for EEVDF
    bigtime_t VirtualDeadline() const { return fVirtualDeadline; }
    void SetVirtualDeadline(bigtime_t deadline) { fVirtualDeadline = deadline; }

private:
    bigtime_t fVirtualDeadline;
};

#endif // THREAD_DATA_H
