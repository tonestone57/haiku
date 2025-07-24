#ifndef THREAD_DATA_H
#define THREAD_DATA_H

#include <stdint.h>

class EevdfThreadData {
public:
    EevdfThreadData(int id, int64_t deadline)
        : _id(id),
          _virtualDeadline(deadline),
          _runtime(0)
    {}

    int ID() const { return _id; }

    // The deadline used by EEVDF scheduler
    int64_t VirtualDeadline() const { return _virtualDeadline; }

    void SetVirtualDeadline(int64_t deadline) {
        _virtualDeadline = deadline;
    }

    int64_t Runtime() const { return _runtime; }

    void AddRuntime(int64_t delta) {
        _runtime += delta;
    }

private:
    int _id;                    // Unique thread identifier
    int64_t _virtualDeadline;   // For EEVDF prioritization
    int64_t _runtime;           // Accumulated run time
};

#endif // THREAD_DATA_H
