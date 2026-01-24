/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// TODO - see PLplot plplot.sourceforge.net/download.phpd for visualization


#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "slikenet/GetTime.h"
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/StatisticsHistory.h"
#include <math.h>
#include "slikenet/sleep.h"

using namespace MafiaNet;

enum HistoryObject
{
	HO_SIN_WAVE,
	HO_COS_WAVE,
};
static const int numGraphColumns=79;
static const int numGraphRows=5;

void PrintGraph(DataStructures::Queue<StatisticsHistory::TimeAndValue> &histogram, SHValueType highest, SHValueType lowest)
{
	if (histogram.Size()==0)
		return;
	

	SHValueType range = highest - lowest;
	bool drawPoint[numGraphRows][numGraphColumns];
	memset(drawPoint, 0, sizeof(drawPoint));

	unsigned int numR;

	for (unsigned int col = 0; col < histogram.Size() && col < numGraphColumns; col++)
	{
		double r = (double) numGraphRows * ((histogram[col].val-lowest) / range);
		if (r - floor(r)>.5)
			r=ceil(r);
		else
			r=floor(r);
		numR = (unsigned int) r;
		for (unsigned int row = 0; row < numR && row < numGraphRows; row++)
		{
			drawPoint[row][col]=true;
		}
	}

	for (int row=numGraphRows-1; row >= 0; row--)
	{
		for (unsigned int col = 0; col < numGraphColumns; col++)
		{
			if (drawPoint[row][col])
				printf("|");
			else
				printf(" ");
		}
		printf("\n");
	}
	for (unsigned int col = 0; col < numGraphColumns; col++)
		printf("-");
	printf("\n\n");
}
int main(void)
{
	printf("Tests StatisticsHistory in single player.\n");
	printf("Difficulty: Intermediate\n\n");

	DataStructures::Queue<StatisticsHistory::TimeAndValue> histogram;
	StatisticsHistory::TimeAndValueQueue *tav;
	StatisticsHistory::TimeAndValueQueue tavInst;
	StatisticsHistory statisticsHistory;
	statisticsHistory.SetDefaultTimeToTrack(10000);
	statisticsHistory.AddObject(StatisticsHistory::TrackedObjectData(HO_SIN_WAVE,0,0));
	statisticsHistory.AddObject(StatisticsHistory::TrackedObjectData(HO_COS_WAVE,0,0));
	double f;

	Time nextPrint=0;
	for(;;)
	{
		f = (double) ((double)GetTime() / (double)1000);
		statisticsHistory.AddValueByObjectID(HO_SIN_WAVE,"Waveform",sin(f),GetTime(), false);
		statisticsHistory.AddValueByObjectID(HO_COS_WAVE,"Waveform",cos(f),GetTime(), false);

		// Show sin wave
		if (GetTime()>nextPrint)
		{
			system("cls");

			Time curTime = GetTime();

			statisticsHistory.GetHistoryForKey(HO_SIN_WAVE, "Waveform", &tav, curTime);
			tav->ResizeSampleSet(numGraphColumns, histogram, StatisticsHistory::DC_CONTINUOUS);
			PrintGraph(histogram, 1, -1);


			// Show cos wave
			statisticsHistory.GetHistoryForKey(HO_COS_WAVE, "Waveform", &tav, curTime);
			tav->ResizeSampleSet(numGraphColumns, histogram, StatisticsHistory::DC_CONTINUOUS);
			PrintGraph(histogram, 1, -1);

			nextPrint = GetTime() + 500;

			// Show sin wave + cos wave
			statisticsHistory.MergeAllObjectsOnKey("Waveform", &tavInst, StatisticsHistory::DC_CONTINUOUS);
			tavInst.ResizeSampleSet(numGraphColumns, histogram, StatisticsHistory::DC_CONTINUOUS);
			PrintGraph(histogram, 2, -2);
		}

		RakSleep(30);
	}
	

	return 1;
}

