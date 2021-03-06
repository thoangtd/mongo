/*
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {
    /**
     * A simple thread-safe fail point implementation that can be activated and
     * deactivated, as well as embed temporary data into it.
     *
     * The fail point has a static instance, which is represented by a FailPoint
     * object, and dynamic instance, which are all the threads in between
     * shouldFailOpenBlock and shouldFailCloseBlock.
     *
     * Sample use:
     * // Declared somewhere:
     * FailPoint makeBadThingsHappen;
     *
     * // Somewhere in the code
     * return false || MONGO_FAIL_POINT(makeBadThingsHappen);
     *
     * // MONGO_FAIL_POINT bad usage example
     * if (MONGO_FAIL_POINT(makeBadThingsHappen)) {
     *     // Important! - You must not call getData() here.
     *     // Use MONGO_FAIL_POINT_BLOCK instead if you need to.
     *     const BSONObj& data = makeBadThingsHappen.getData();
     * }
     *
     * or
     *
     * // Somewhere in the code
     * MONGO_FAIL_POINT_BLOCK(makeBadThingsHappen) {
     *     const BSONObj& data = makeBadThingsHappen.getData();
     *     // Do something
     * }
     *
     * Invariants:
     *
     * 1. Always refer to _fpInfo first to check if failPoint is active or not before
     *    entering fail point or modifying fail point.
     * 2. Client visible fail point states are read-only when active.
     */
    class FailPoint {
    public:
        typedef AtomicUInt32::WordType ValType;
        enum Mode { off, alwaysOn, random, nTimes, numModes };
        enum RetCode { fastOff = 0, slowOff, slowOn };

        FailPoint();

        /**
         * Note: This is not side-effect free - it can change the state to OFF after calling.
         *
         * @return true if fail point is active.
         */
        inline bool shouldFail() {
            RetCode ret = shouldFailOpenBlock();

            if (MONGO_likely(ret == fastOff)) {
                return false;
            }

            shouldFailCloseBlock();
            return ret == slowOn;
        }

        /**
         * Checks whether fail point is active and increments the reference counter without
         * decrementing it. Must call shouldFailCloseBlock afterwards when the return value
         * is not fastOff. Otherwise, this will remain read-only forever.
         *
         * @return slowOn if fail point is active.
         */
        inline RetCode shouldFailOpenBlock() {
            // TODO: optimization - use unordered load once available
            if (MONGO_likely((_fpInfo.load() & ACTIVE_BIT) == 0)) {
                return fastOff;
            }

            return slowShouldFailOpenBlock();
        }

        /**
         * Decrements the reference counter.
         * @see #shouldFailOpenBlock
         */
        void shouldFailCloseBlock();

        /**
         * Changes the settings of this fail point. This will turn off the fail point
         * and waits for all dynamic instances referencing this fail point to go away before
         * actually modifying the settings.
         *
         * @param mode the new mode for this fail point.
         * @param val the value that can have different usage depending on the mode:
         *
         *     - off, alwaysOn: ignored
         *     - random:
         *     - nTimes: the number of times this fail point will be active when
         *         #shouldFail or #shouldFailOpenBlock is called.
         *
         * @param extra arbitrary BSON object that can be stored to this fail point
         *     that can be referenced afterwards with #getData. Defaults to an empty
         *     document.
         */
        void setMode(Mode mode, ValType val = 0, const BSONObj& extra = BSONObj());

        /**
         * @return the stored BSONObj in this fail point. Note that this cannot be safely
         *      read if this fail point is off.
         */
        const BSONObj& getData() const;

    private:
        static const ValType ACTIVE_BIT = 1 << 31;
        static const ValType REF_COUNTER_MASK = ~ACTIVE_BIT;

        // Bit layout:
        // 31: tells whether this fail point is active.
        // 0~30: unsigned ref counter for active dynamic instances.
        AtomicUInt32 _fpInfo;

        // Invariant: These should be read only if ACTIVE_BIT of _fpInfo is set.
        Mode _mode;
        AtomicInt32 _timesOrPeriod;
        BSONObj _data;

        // protects _mode, _timesOrPeriod, _data
        mutex _modMutex;

        /**
         * Disables this fail point.
         */
        void disableFailPoint();

        /**
         * slow path for #shouldFailOpenBlock
         */
        RetCode slowShouldFailOpenBlock();
    };

    /**
     * Helper class for making sure that FailPoint#shouldFailCloseBlock is called when
     * FailPoint#shouldFailOpenBlock was called.
     */
    class ScopedFailPoint {
    public:
        ScopedFailPoint(FailPoint* failPoint);
        ~ScopedFailPoint();

        /**
         * @return true if fail point is on. This will be true at most once.
         */
        inline bool isActive() {
            if (_once) {
                return false;
            }

            _once = true;

            FailPoint::RetCode ret = _failPoint->shouldFailOpenBlock();
            _shouldClose = ret != FailPoint::fastOff;
            return ret == FailPoint::slowOn;
        }

    private:
        FailPoint* _failPoint;
        bool _once;
        bool _shouldClose;
    };

    #define MONGO_FAIL_POINT(symbol) MONGO_unlikely(symbol.shouldFail())
    #define MONGO_FAIL_POINT_BLOCK(symbol) for (mongo::ScopedFailPoint scopedFP(&symbol); \
        MONGO_unlikely(scopedFP.isActive()); )
}
