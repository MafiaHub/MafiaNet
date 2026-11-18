/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/native_feature_includes.h"
#if _RAKNET_SUPPORT_StatisticsHistory == 1

#include "mafianet/statistics_history.h"
#include "mafianet/get_time.h"
#include "mafianet/statistics.h"
#include "mafianet/peer_interface.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace MafiaNet;

STATIC_FACTORY_DEFINITIONS(StatisticsHistory, StatisticsHistory);
STATIC_FACTORY_DEFINITIONS(StatisticsHistoryPlugin, StatisticsHistoryPlugin);

int StatisticsHistory::TrackedObjectComp(const uint64_t &key, StatisticsHistory::TrackedObject *const &data)
{
	if (key < data->trackedObjectData.objectId)
		return -1;
	if (key == data->trackedObjectData.objectId)
		return 0;
	return 1;
}

// A NaN sort value (a NaN sample, or inf - inf after culling) is unordered against everything, which
// would break std::sort's strict weak ordering. Both orders below put NaN after every number (NaNs
// tie with each other and fall back to the key), so the order is total whatever the values are.
// \return The NaN part of the comparison of a and b (-1, 1), or 0 if neither or both are NaN
static int CompareNaNLast(SHValueType a, SHValueType b)
{
	const bool aIsNaN = std::isnan(a);
	const bool bIsNaN = std::isnan(b);
	if (aIsNaN == bIsNaN)
		return 0;
	return aIsNaN ? 1 : -1;
}

static int TimeAndValueQueueCompAsc(StatisticsHistory::TimeAndValueQueue *const &key, StatisticsHistory::TimeAndValueQueue *const &data)
{
	const int nanOrder = CompareNaNLast(key->sortValue, data->sortValue);
	if (nanOrder != 0)
		return nanOrder;
	if (key->sortValue < data->sortValue)
		return -1;
	if (key->sortValue > data->sortValue)
		return 1;
	if (key->key < data->key)
		return -1;
	if (key->key > data->key)
		return 1;
	return 0;
}

static int TimeAndValueQueueCompDesc(StatisticsHistory::TimeAndValueQueue *const &key, StatisticsHistory::TimeAndValueQueue *const &data)
{
	const int nanOrder = CompareNaNLast(key->sortValue, data->sortValue);
	if (nanOrder != 0)
		return nanOrder;
	if (key->sortValue > data->sortValue)
		return -1;
	if (key->sortValue < data->sortValue)
		return 1;
	if (key->key > data->key)
		return -1;
	if (key->key < data->key)
		return 1;
	return 0;
}
StatisticsHistory::TrackedObjectData::TrackedObjectData() {}
StatisticsHistory::TrackedObjectData::TrackedObjectData(uint64_t _objectId, int _objectType, void *_userData)
{
	objectId = _objectId;
	objectType = _objectType;
	userData = _userData;
}
StatisticsHistory::StatisticsHistory()
{
	timeToTrack = 30000;
}
StatisticsHistory::~StatisticsHistory()
{
	Clear();
}
void StatisticsHistory::SetDefaultTimeToTrack(Time defaultTimeToTrack)
{
	timeToTrack = defaultTimeToTrack;
}
Time StatisticsHistory::GetDefaultTimeToTrack(void) const
{
	return timeToTrack;
}
std::vector<std::unique_ptr<StatisticsHistory::TrackedObject>>::const_iterator StatisticsHistory::FindObject(uint64_t objectId) const
{
	const auto position = std::lower_bound(objects.begin(), objects.end(), objectId, [](const std::unique_ptr<TrackedObject> &entry, uint64_t id)
	                                       { return TrackedObjectComp(id, entry.get()) > 0; });
	if (position != objects.end() && (*position)->trackedObjectData.objectId == objectId)
		return position;
	return objects.end();
}
bool StatisticsHistory::AddObject(TrackedObjectData tod)
{
	const auto position = std::lower_bound(objects.begin(), objects.end(), tod.objectId, [](const std::unique_ptr<TrackedObject> &entry, uint64_t id)
	                                       { return TrackedObjectComp(id, entry.get()) > 0; });
	if (position != objects.end() && (*position)->trackedObjectData.objectId == tod.objectId)
		return false;
	TrackedObject *to = new TrackedObject;
	to->trackedObjectData = tod;
	objects.emplace(position, to);
	return true;
}
bool StatisticsHistory::RemoveObject(uint64_t objectId, void **userData)
{
	unsigned int idx = GetObjectIndex(objectId);
	if (idx == (unsigned int) -1)
		return false;
	if (userData)
		*userData = objects[idx]->trackedObjectData.userData;
	RemoveObjectAtIndex(idx);
	return true;
}
void StatisticsHistory::RemoveObjectAtIndex(unsigned int index)
{
	objects.erase(objects.begin() + index);
}
void StatisticsHistory::Clear(void)
{
	objects.clear();
}
unsigned int StatisticsHistory::GetObjectCount(void) const
{
	return (unsigned int) objects.size();
}
StatisticsHistory::TrackedObjectData *StatisticsHistory::GetObjectAtIndex(unsigned int index) const
{
	return &objects[index]->trackedObjectData;
}
bool StatisticsHistory::AddValueByObjectID(uint64_t objectId, const std::string &key, SHValueType val, Time curTime, bool combineEqualTimes)
{
	unsigned int idx = GetObjectIndex(objectId);
	if (idx == (unsigned int) -1)
		return false;
	AddValueByIndex(idx, key, val, curTime, combineEqualTimes);
	return true;
}
void StatisticsHistory::AddValueByIndex(unsigned int index, const std::string &key, SHValueType val, Time curTime, bool combineEqualTimes)
{
	TrackedObject *to = objects[index].get();
	std::unique_ptr<TimeAndValueQueue> &slot = to->dataQueues[key];
	if (!slot)
	{
		slot.reset(new TimeAndValueQueue);
		slot->key = key;
		slot->timeToTrackValues = timeToTrack;
	}
	TimeAndValueQueue *queue = slot.get();

	TimeAndValue tav;
	if (combineEqualTimes == true && !queue->values.empty() && queue->values.back().time == curTime)
	{
		tav = queue->values.back();
		queue->values.pop_back();

		queue->recentSum -= tav.val;
		queue->recentSumOfSquares -= tav.val * tav.val;
		queue->longTermSum -= tav.val;
		queue->longTermCount = queue->longTermCount - 1;
	}
	else
	{
		tav.val = 0.0;
		tav.time = curTime;
	}

	tav.val += val;
	queue->values.push_back(tav);

	queue->recentSum += tav.val;
	queue->recentSumOfSquares += tav.val * tav.val;
	queue->longTermSum += tav.val;
	queue->longTermCount = queue->longTermCount + 1;
	if (queue->longTermLowest > tav.val)
		queue->longTermLowest = tav.val;
	if (queue->longTermHighest < tav.val)
		queue->longTermHighest = tav.val;
}
StatisticsHistory::SHErrorCode StatisticsHistory::GetHistoryForKey(uint64_t objectId, const std::string &key, StatisticsHistory::TimeAndValueQueue **values, Time curTime) const
{
	if (values == 0)
		return SH_INVALID_PARAMETER;

	unsigned int idx = GetObjectIndex(objectId);
	if (idx == (unsigned int) -1)
		return SH_UKNOWN_OBJECT;
	TrackedObject *to = objects[idx].get();
	const auto queue = to->dataQueues.find(key);
	if (queue == to->dataQueues.end())
		return SH_UKNOWN_KEY;
	*values = queue->second.get();
	(*values)->CullExpiredValues(curTime);
	return SH_OK;
}
bool StatisticsHistory::GetHistorySorted(uint64_t objectId, SHSortOperation sortType, std::vector<StatisticsHistory::TimeAndValueQueue *> &values) const
{
	unsigned int idx = GetObjectIndex(objectId);
	if (idx == (unsigned int) -1)
		return false;
	TrackedObject *to = objects[idx].get();
	Time curTime = GetTime();

	const bool ascending =
	    sortType == SH_SORT_BY_RECENT_SUM_ASCENDING ||
	    sortType == SH_SORT_BY_LONG_TERM_SUM_ASCENDING ||
	    sortType == SH_SORT_BY_RECENT_SUM_OF_SQUARES_ASCENDING ||
	    sortType == SH_SORT_BY_RECENT_AVERAGE_ASCENDING ||
	    sortType == SH_SORT_BY_LONG_TERM_AVERAGE_ASCENDING ||
	    sortType == SH_SORT_BY_RECENT_HIGHEST_ASCENDING ||
	    sortType == SH_SORT_BY_RECENT_LOWEST_ASCENDING ||
	    sortType == SH_SORT_BY_LONG_TERM_HIGHEST_ASCENDING ||
	    sortType == SH_SORT_BY_LONG_TERM_LOWEST_ASCENDING;

	std::vector<TimeAndValueQueue *> sortedQueues;
	sortedQueues.reserve(to->dataQueues.size());
	for (const auto &entry : to->dataQueues)
	{
		TimeAndValueQueue *tavq = entry.second.get();
		tavq->CullExpiredValues(curTime);

		if (sortType == SH_SORT_BY_RECENT_SUM_ASCENDING || sortType == SH_SORT_BY_RECENT_SUM_DESCENDING)
			tavq->sortValue = tavq->GetRecentSum();
		else if (sortType == SH_SORT_BY_LONG_TERM_SUM_ASCENDING || sortType == SH_SORT_BY_LONG_TERM_SUM_DESCENDING)
			tavq->sortValue = tavq->GetLongTermSum();
		else if (sortType == SH_SORT_BY_RECENT_SUM_OF_SQUARES_ASCENDING || sortType == SH_SORT_BY_RECENT_SUM_OF_SQUARES_DESCENDING)
			tavq->sortValue = tavq->GetRecentSumOfSquares();
		else if (sortType == SH_SORT_BY_RECENT_AVERAGE_ASCENDING || sortType == SH_SORT_BY_RECENT_AVERAGE_DESCENDING)
			tavq->sortValue = tavq->GetRecentAverage();
		else if (sortType == SH_SORT_BY_LONG_TERM_AVERAGE_ASCENDING || sortType == SH_SORT_BY_LONG_TERM_AVERAGE_DESCENDING)
			tavq->sortValue = tavq->GetLongTermAverage();
		else if (sortType == SH_SORT_BY_RECENT_HIGHEST_ASCENDING || sortType == SH_SORT_BY_RECENT_HIGHEST_DESCENDING)
			tavq->sortValue = tavq->GetRecentHighest();
		else if (sortType == SH_SORT_BY_RECENT_LOWEST_ASCENDING || sortType == SH_SORT_BY_RECENT_LOWEST_DESCENDING)
			tavq->sortValue = tavq->GetRecentLowest();
		else if (sortType == SH_SORT_BY_LONG_TERM_HIGHEST_ASCENDING || sortType == SH_SORT_BY_LONG_TERM_HIGHEST_DESCENDING)
			tavq->sortValue = tavq->GetLongTermHighest();
		else
			tavq->sortValue = tavq->GetLongTermLowest();

		sortedQueues.push_back(tavq);
	}

	// Keys are unique within an object, so the order is total.
	if (ascending)
		std::sort(sortedQueues.begin(), sortedQueues.end(), [](TimeAndValueQueue *a, TimeAndValueQueue *b)
		          { return TimeAndValueQueueCompAsc(a, b) < 0; });
	else
		std::sort(sortedQueues.begin(), sortedQueues.end(), [](TimeAndValueQueue *a, TimeAndValueQueue *b)
		          { return TimeAndValueQueueCompDesc(a, b) < 0; });

	values.insert(values.end(), sortedQueues.begin(), sortedQueues.end());
	return true;
}
void StatisticsHistory::MergeAllObjectsOnKey(const std::string &key, TimeAndValueQueue *tavqOutput, SHDataCategory dataCategory) const
{
	tavqOutput->Clear();

	Time curTime = GetTime();

	// Find every object with this key
	for (const std::unique_ptr<TrackedObject> &to : objects)
	{
		const auto queue = to->dataQueues.find(key);
		if (queue != to->dataQueues.end())
		{
			TimeAndValueQueue *tavqInput = queue->second.get();
			tavqInput->CullExpiredValues(curTime);
			TimeAndValueQueue::MergeSets(tavqOutput, dataCategory, tavqInput, dataCategory, tavqOutput);
		}
	}
}
void StatisticsHistory::GetUniqueKeyList(std::vector<std::string> &keys)
{
	keys.clear();

	for (const std::unique_ptr<TrackedObject> &to : objects)
	{
		for (const auto &entry : to->dataQueues)
		{
			if (std::find(keys.begin(), keys.end(), entry.first) == keys.end())
				keys.push_back(entry.first);
		}
	}
}
StatisticsHistory::TimeAndValueQueue::TimeAndValueQueue()
{
	Clear();
}
StatisticsHistory::TimeAndValueQueue::~TimeAndValueQueue() {}
void StatisticsHistory::TimeAndValueQueue::SetTimeToTrackValues(Time t)
{
	timeToTrackValues = t;
}
Time StatisticsHistory::TimeAndValueQueue::GetTimeToTrackValues(void) const
{
	return timeToTrackValues;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentSum(void) const
{
	return recentSum;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentSumOfSquares(void) const
{
	return recentSumOfSquares;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetLongTermSum(void) const
{
	return longTermSum;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentAverage(void) const
{
	if (!values.empty())
		return recentSum / (SHValueType) values.size();
	else
		return 0;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentLowest(void) const
{
	SHValueType out = SH_TYPE_MAX;
	for (const TimeAndValue &value : values)
	{
		if (value.val < out)
			out = value.val;
	}
	return out;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentHighest(void) const
{
	SHValueType out = -SH_TYPE_MAX;
	for (const TimeAndValue &value : values)
	{
		if (value.val > out)
			out = value.val;
	}
	return out;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetRecentStandardDeviation(void) const
{
	if (values.empty())
		return 0;

	SHValueType recentMean = GetRecentAverage();
	SHValueType squareOfMean = recentMean * recentMean;
	SHValueType meanOfSquares = GetRecentSumOfSquares() / (SHValueType) values.size();
	return meanOfSquares - squareOfMean;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetLongTermAverage(void) const
{
	if (longTermCount == 0)
		return 0;
	return longTermSum / longTermCount;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetLongTermLowest(void) const
{
	return longTermLowest;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetLongTermHighest(void) const
{
	return longTermHighest;
}
Time StatisticsHistory::TimeAndValueQueue::GetTimeRange(void) const
{
	if (values.size() < 2)
		return 0;
	return values.back().time - values.front().time;
}
SHValueType StatisticsHistory::TimeAndValueQueue::GetSumSinceTime(Time t) const
{
	SHValueType sum = 0;
	for (size_t i = values.size(); i > 0; --i)
	{
		if (values[i - 1].time >= t)
			sum += values[i - 1].val;
	}
	return sum;
}
void StatisticsHistory::TimeAndValueQueue::MergeSets(const TimeAndValueQueue *lhs, SHDataCategory lhsDataCategory, const TimeAndValueQueue *rhs, SHDataCategory rhsDataCategory, TimeAndValueQueue *output)
{
	// Two ways to merge:
	// 1. Treat rhs as just more data points.
	// 1A. Sums are just added. If two values have the same time, just put in queue twice
	// 1B. longTermLowest and longTermHighest are the lowest and highest of the two sets
	//
	// 2. Add by time. If time for the other set is missing, calculate slope to extrapolate
	// 2A. Have to recalculate recentSum, recentSumOfSquares.
	// 2B. longTermSum, longTermCount, longTermLowest, longTermHighest are unknown

	if (lhs != output)
	{
		output->key = lhs->key;
		output->timeToTrackValues = lhs->timeToTrackValues;
	}
	else
	{
		output->key = rhs->key;
		output->timeToTrackValues = rhs->timeToTrackValues;
	}

	size_t lhsIndex, rhsIndex;
	lhsIndex = 0;
	rhsIndex = 0;

	// I use local valuesOutput in case lhs==output || rhs==output
	std::deque<TimeAndValue> valuesOutput;

	if (lhsDataCategory == StatisticsHistory::DC_DISCRETE && rhsDataCategory == StatisticsHistory::DC_DISCRETE)
	{
		// A time-ordered merge. lhs used to be indexed with rhsIndex here, reading the wrong
		// sample, or past the end of lhs->values.
		while (rhsIndex < rhs->values.size() && lhsIndex < lhs->values.size())
		{
			if (rhs->values[rhsIndex].time < lhs->values[lhsIndex].time)
			{
				valuesOutput.push_back(rhs->values[rhsIndex]);
				rhsIndex++;
			}
			else if (rhs->values[rhsIndex].time > lhs->values[lhsIndex].time)
			{
				valuesOutput.push_back(lhs->values[lhsIndex]);
				lhsIndex++;
			}
			else
			{
				valuesOutput.push_back(rhs->values[rhsIndex]);
				rhsIndex++;
				valuesOutput.push_back(lhs->values[lhsIndex]);
				lhsIndex++;
			}
		}

		while (rhsIndex < rhs->values.size())
		{
			valuesOutput.push_back(rhs->values[rhsIndex]);
			rhsIndex++;
		}
		while (lhsIndex < lhs->values.size())
		{
			valuesOutput.push_back(lhs->values[lhsIndex]);
			lhsIndex++;
		}

		output->recentSum = lhs->recentSum + rhs->recentSum;
		output->recentSumOfSquares = lhs->recentSumOfSquares + rhs->recentSumOfSquares;
		output->longTermSum = lhs->longTermSum + rhs->longTermSum;
		output->longTermCount = lhs->longTermCount + rhs->longTermCount;
		if (lhs->longTermLowest < rhs->longTermLowest)
			output->longTermLowest = lhs->longTermLowest;
		else
			output->longTermLowest = rhs->longTermLowest;
		if (lhs->longTermHighest > rhs->longTermHighest)
			output->longTermHighest = lhs->longTermHighest;
		else
			output->longTermHighest = rhs->longTermHighest;
	}
	else
	{
		TimeAndValue lastTimeAndValueLhs, lastTimeAndValueRhs;
		lastTimeAndValueLhs.time = 0;
		lastTimeAndValueLhs.val = 0;
		lastTimeAndValueRhs.time = 0;
		lastTimeAndValueRhs.val = 0;
		SHValueType lastSlopeLhs = 0;
		SHValueType lastSlopeRhs = 0;
		Time timeSinceOppositeValue;

		TimeAndValue newTimeAndValue;

		while (rhsIndex < rhs->values.size() && lhsIndex < lhs->values.size())
		{
			if (rhs->values[rhsIndex].time < lhs->values[lhsIndex].time)
			{
				timeSinceOppositeValue = rhs->values[rhsIndex].time - lastTimeAndValueLhs.time;
				newTimeAndValue.val = rhs->values[rhsIndex].val + lastTimeAndValueLhs.val + lastSlopeLhs * timeSinceOppositeValue;
				newTimeAndValue.time = rhs->values[rhsIndex].time;
				lastTimeAndValueRhs = rhs->values[rhsIndex];
				if (rhsIndex > 0 && rhs->values[rhsIndex].time != rhs->values[rhsIndex - 1].time && rhsDataCategory == StatisticsHistory::DC_CONTINUOUS)
					lastSlopeRhs = (rhs->values[rhsIndex].val - rhs->values[rhsIndex - 1].val) / (SHValueType) (rhs->values[rhsIndex].time - rhs->values[rhsIndex - 1].time);
				rhsIndex++;
			}
			else if (lhs->values[lhsIndex].time < rhs->values[rhsIndex].time)
			{
				timeSinceOppositeValue = lhs->values[lhsIndex].time - lastTimeAndValueRhs.time;
				newTimeAndValue.val = lhs->values[lhsIndex].val + lastTimeAndValueRhs.val + lastSlopeRhs * timeSinceOppositeValue;
				newTimeAndValue.time = lhs->values[lhsIndex].time;
				lastTimeAndValueLhs = lhs->values[lhsIndex];
				if (lhsIndex > 0 && lhs->values[lhsIndex].time != lhs->values[lhsIndex - 1].time && lhsDataCategory == StatisticsHistory::DC_CONTINUOUS)
					lastSlopeLhs = (lhs->values[lhsIndex].val - lhs->values[lhsIndex - 1].val) / (SHValueType) (lhs->values[lhsIndex].time - lhs->values[lhsIndex - 1].time);
				lhsIndex++;
			}
			else
			{
				newTimeAndValue.val = lhs->values[lhsIndex].val + rhs->values[rhsIndex].val;
				newTimeAndValue.time = lhs->values[lhsIndex].time;
				lastTimeAndValueRhs = rhs->values[rhsIndex];
				lastTimeAndValueLhs = lhs->values[lhsIndex];
				if (rhsIndex > 0 && rhs->values[rhsIndex].time != rhs->values[rhsIndex - 1].time && rhsDataCategory == StatisticsHistory::DC_CONTINUOUS)
					lastSlopeRhs = (rhs->values[rhsIndex].val - rhs->values[rhsIndex - 1].val) / (SHValueType) (rhs->values[rhsIndex].time - rhs->values[rhsIndex - 1].time);
				if (lhsIndex > 0 && lhs->values[lhsIndex].time != lhs->values[lhsIndex - 1].time && lhsDataCategory == StatisticsHistory::DC_CONTINUOUS)
					lastSlopeLhs = (lhs->values[lhsIndex].val - lhs->values[lhsIndex - 1].val) / (SHValueType) (lhs->values[lhsIndex].time - lhs->values[lhsIndex - 1].time);
				lhsIndex++;
				rhsIndex++;
			}

			valuesOutput.push_back(newTimeAndValue);
		}

		while (rhsIndex < rhs->values.size())
		{
			timeSinceOppositeValue = rhs->values[rhsIndex].time - lastTimeAndValueLhs.time;
			newTimeAndValue.val = rhs->values[rhsIndex].val + lastTimeAndValueLhs.val + lastSlopeLhs * timeSinceOppositeValue;
			newTimeAndValue.time = rhs->values[rhsIndex].time;
			valuesOutput.push_back(newTimeAndValue);
			rhsIndex++;
		}
		while (lhsIndex < lhs->values.size())
		{
			timeSinceOppositeValue = lhs->values[lhsIndex].time - lastTimeAndValueRhs.time;
			newTimeAndValue.val = lhs->values[lhsIndex].val + lastTimeAndValueRhs.val + lastSlopeRhs * timeSinceOppositeValue;
			newTimeAndValue.time = lhs->values[lhsIndex].time;
			valuesOutput.push_back(newTimeAndValue);
			lhsIndex++;
		}

		output->recentSum = 0;
		output->recentSumOfSquares = 0;
		for (const TimeAndValue &value : valuesOutput)
		{
			output->recentSum += value.val;
			output->recentSumOfSquares += value.val * value.val;
		}
	}

	output->values = std::move(valuesOutput);
}
void StatisticsHistory::TimeAndValueQueue::ResizeSampleSet(int maxSamples, std::deque<StatisticsHistory::TimeAndValue> &histogram, SHDataCategory dataCategory, Time timeClipStart, Time timeClipEnd)
{
	histogram.clear();
	if (maxSamples == 0)
		return;
	Time timeRange = GetTimeRange();
	if (timeRange == 0)
		return;
	if (maxSamples == 1)
	{
		StatisticsHistory::TimeAndValue tav;
		tav.time = timeRange;
		tav.val = GetRecentSum();
		histogram.push_back(tav);
		return;
	}
	Time interval = timeRange / maxSamples;
	if (interval == 0)
		interval = 1;
	size_t dataIndex;
	Time timeBoundary;
	StatisticsHistory::TimeAndValue currentSum;
	SHValueType numSamples;
	Time endTime;

	numSamples = 0;
	endTime = values.back().time;
	dataIndex = 0;
	currentSum.val = 0;
	currentSum.time = values[0].time + interval / 2;
	timeBoundary = values[0].time + interval;
	while (timeBoundary <= endTime)
	{
		while (dataIndex < values.size() && values[dataIndex].time <= timeBoundary)
		{
			currentSum.val += values[dataIndex].val;
			dataIndex++;
			numSamples++;
		}

		if (dataCategory == DC_CONTINUOUS)
		{
			if (dataIndex > 0 &&
			    dataIndex < values.size() &&
			    values[dataIndex - 1].time < timeBoundary &&
			    values[dataIndex].time > timeBoundary)
			{
				SHValueType interpolatedValue = Interpolate(values[dataIndex - 1], values[dataIndex], timeBoundary);
				currentSum.val += interpolatedValue;
				numSamples++;
			}

			if (numSamples > 1)
			{
				currentSum.val /= numSamples;
			}
		}

		histogram.push_back(currentSum);
		currentSum.time = timeBoundary + interval / 2;
		timeBoundary += interval;
		currentSum.val = 0;
		numSamples = 0;
	}


	if (timeClipStart != 0 && !histogram.empty())
	{
		timeClipStart = histogram.front().time + timeClipStart;
		if (histogram.back().time < timeClipStart)
		{
			histogram.clear();
		}
		else if (histogram.size() >= 2 && histogram.front().time < timeClipStart)
		{
			StatisticsHistory::TimeAndValue tav;

			do
			{
				tav = histogram.front();
				histogram.pop_front();

				if (histogram.front().time == timeClipStart)
				{
					break;
				}
				else if (histogram.front().time > timeClipStart)
				{
					StatisticsHistory::TimeAndValue tav2;
					tav2.val = StatisticsHistory::TimeAndValueQueue::Interpolate(tav, histogram.front(), timeClipStart);
					tav2.time = timeClipStart;
					histogram.push_front(tav2);
					break;
				}
			} while (histogram.size() >= 2);
		}
	}

	if (timeClipEnd != 0 && !histogram.empty())
	{
		timeClipEnd = histogram.back().time - timeClipEnd;
		if (histogram.front().time > timeClipEnd)
		{
			histogram.clear();
		}
		else if (histogram.size() >= 2 && histogram.back().time > timeClipEnd)
		{
			StatisticsHistory::TimeAndValue tav;

			do
			{
				tav = histogram.back();
				histogram.pop_back();

				if (histogram.back().time == timeClipEnd)
				{
					break;
				}
				else if (histogram.back().time < timeClipEnd)
				{
					StatisticsHistory::TimeAndValue tav2;
					tav2.val = StatisticsHistory::TimeAndValueQueue::Interpolate(tav, histogram.back(), timeClipEnd);
					tav2.time = timeClipEnd;
					histogram.push_back(tav2);
					break;
				}
			} while (histogram.size() >= 2);
		}
	}
}
void StatisticsHistory::TimeAndValueQueue::CullExpiredValues(Time curTime)
{
	while (!values.empty())
	{
		StatisticsHistory::TimeAndValue tav = values.front();
		if (curTime - tav.time > timeToTrackValues)
		{
			recentSum -= tav.val;
			recentSumOfSquares -= tav.val * tav.val;
			values.pop_front();
		}
		else
		{
			break;
		}
	}
}
SHValueType StatisticsHistory::TimeAndValueQueue::Interpolate(StatisticsHistory::TimeAndValue t1, StatisticsHistory::TimeAndValue t2, Time time)
{
	if (t2.time == t1.time)
		return (t1.val + t2.val) / 2;
	//	if (t2.time > t1.time)
	//	{
	SHValueType slope = (t2.val - t1.val) / ((SHValueType) t2.time - (SHValueType) t1.time);
	return t1.val + slope * ((SHValueType) time - (SHValueType) t1.time);
	// 	}
	// 	else
	// 	{
	// 		SHValueType slope = (t1.val - t2.val) / (SHValueType) (t1.time - t2.time);
	// 		return t2.val + slope * (SHValueType) (time - t2.time);
	// 	}
}
void StatisticsHistory::TimeAndValueQueue::Clear(void)
{
	recentSum = 0;
	recentSumOfSquares = 0;
	longTermSum = 0;
	longTermCount = 0;
	longTermLowest = SH_TYPE_MAX;
	longTermHighest = -SH_TYPE_MAX;
	values.clear();
}
StatisticsHistory::TimeAndValueQueue &StatisticsHistory::TimeAndValueQueue::operator=(const TimeAndValueQueue &input)
{
	values = input.values;
	timeToTrackValues = input.timeToTrackValues;
	key = input.key;
	recentSum = input.recentSum;
	recentSumOfSquares = input.recentSumOfSquares;
	longTermSum = input.longTermSum;
	longTermCount = input.longTermCount;
	longTermLowest = input.longTermLowest;
	longTermHighest = input.longTermHighest;
	return *this;
}
StatisticsHistory::TrackedObject::TrackedObject() {}
// The queues are owned by dataQueues. The old destructor walked an empty list and leaked them.
StatisticsHistory::TrackedObject::~TrackedObject() = default;
unsigned int StatisticsHistory::GetObjectIndex(uint64_t objectId) const
{
	const auto position = FindObject(objectId);
	if (position == objects.end())
		return (unsigned int) -1;
	return (unsigned int) (position - objects.begin());
}
StatisticsHistoryPlugin::StatisticsHistoryPlugin()
{
	addNewConnections = true;
	removeLostConnections = true;
	newConnectionsObjectType = 0;
	sampleIntervalMs = 0;
	lastSampleTime = 0;
	hasSampled = false;
}
StatisticsHistoryPlugin::~StatisticsHistoryPlugin()
{
}
void StatisticsHistoryPlugin::SetTrackConnections(bool _addNewConnections, int _newConnectionsObjectType, bool _removeLostConnections)
{
	addNewConnections = _addNewConnections;
	removeLostConnections = _removeLostConnections;
	newConnectionsObjectType = _newConnectionsObjectType;
}
void StatisticsHistoryPlugin::SetSampleInterval(Time intervalMs)
{
	sampleIntervalMs = intervalMs;
}
void StatisticsHistoryPlugin::OnRakPeerShutdown(void)
{
	hasSampled = false;
}
void StatisticsHistoryPlugin::Update(void)
{
	UpdateAt(GetTime());
}
bool StatisticsHistoryPlugin::UpdateAt(Time curTime)
{
	// A clock that stepped backwards samples at once rather than stalling until it catches up.
	if (hasSampled && curTime >= lastSampleTime && curTime - lastSampleTime < sampleIntervalMs)
		return false;
	hasSampled = true;
	lastSampleTime = curTime;

	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	std::vector<RakNetStatistics> stats;
	rakPeerInterface->GetStatisticsList(addresses, guids, stats);

	for (unsigned int idx = 0; idx < guids.size(); idx++)
	{
		unsigned int objectIndex = statistics.GetObjectIndex(guids[idx].g);
		if (objectIndex != (unsigned int) -1)
		{
			statistics.AddValueByIndex(objectIndex,
			                           "RN_ACTUAL_BYTES_SENT",
			                           (SHValueType) stats[idx].valueOverLastSecond[ACTUAL_BYTES_SENT],
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_USER_MESSAGE_BYTES_RESENT",
			                           (SHValueType) stats[idx].valueOverLastSecond[USER_MESSAGE_BYTES_RESENT],
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_ACTUAL_BYTES_RECEIVED",
			                           (SHValueType) stats[idx].valueOverLastSecond[ACTUAL_BYTES_RECEIVED],
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_USER_MESSAGE_BYTES_PUSHED",
			                           (SHValueType) stats[idx].valueOverLastSecond[USER_MESSAGE_BYTES_PUSHED],
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_USER_MESSAGE_BYTES_RECEIVED_PROCESSED",
			                           (SHValueType) stats[idx].valueOverLastSecond[USER_MESSAGE_BYTES_RECEIVED_PROCESSED],
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_lastPing",
			                           (SHValueType) rakPeerInterface->GetLastPing(guids[idx]),
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_bytesInResendBuffer",
			                           (SHValueType) stats[idx].bytesInResendBuffer,
			                           curTime, false);

			statistics.AddValueByIndex(objectIndex,
			                           "RN_packetlossLastSecond",
			                           (SHValueType) stats[idx].packetlossLastSecond,
			                           curTime, false);
		}
	}

	/*
	RakNetStatistics rns;
	std::vector<SystemAddress> addresses;
	std::vector<RakNetGUID> guids;
	rakPeerInterface->GetSystemList(addresses, guids);
	for (unsigned int idx = 0; idx < guids.size(); idx++)
	{
		rakPeerInterface->GetStatistics(remoteSystems[idx], &rns);
		statistics.AddValue();

		bool AddValue(uint64_t objectId, std::string key, SHValueType val, Time curTime);

	}
	*/
	return true;
}
/*
void StatisticsHistoryPlugin::OnDirectSocketSend(const char *data, const BitSize_t bitsUsed, SystemAddress remoteSystemAddress)
{
	// Would have to use GetGuidFromSystemAddress for every send
}
void StatisticsHistoryPlugin::OnDirectSocketReceive(const char *data, const BitSize_t bitsUsed, SystemAddress remoteSystemAddress)
{
}
*/
void StatisticsHistoryPlugin::OnClosedConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, PI2_LostConnectionReason lostConnectionReason)
{
	(void) lostConnectionReason;
	(void) systemAddress;

	if (removeLostConnections)
	{
		statistics.RemoveObject(rakNetGUID.g, 0);
	}
}
void StatisticsHistoryPlugin::OnNewConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID, bool isIncoming)
{
	(void) systemAddress;
	(void) isIncoming;

	if (addNewConnections)
	{
		statistics.AddObject(StatisticsHistory::TrackedObjectData(rakNetGUID.g, newConnectionsObjectType, 0));
	}
}
// --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

#endif // _RAKNET_SUPPORT_StatisticsHistory==1
