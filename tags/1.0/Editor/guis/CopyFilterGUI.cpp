/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2015  Jonas Thedering

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "helpers/ChannelHelper.h"
#include "CopyFilterGUIForm.h"
#include "CopyFilterGUI.h"
#include "ui_CopyFilterGUI.h"

using namespace std;

CopyFilterGUI::CopyFilterGUI(CopyFilter* filter) :
	ui(new Ui::CopyFilterGUI)
{
	ui->setupUi(this);

	scene = new CopyFilterGUIScene;
	ui->graphicsView->setScene(scene);
	ui->graphicsView->setBackgroundRole(QPalette::Window);

	ui->form->load(filter->getAssignments());

	connect(scene, SIGNAL(updateModel()), this, SIGNAL(updateModel()));
	connect(scene, SIGNAL(updateChannels()), this, SIGNAL(updateChannels()));

	connect(ui->form, SIGNAL(updateModel()), this, SIGNAL(updateModel()));
	connect(ui->form, SIGNAL(updateChannels()), this, SIGNAL(updateChannels()));
}

CopyFilterGUI::~CopyFilterGUI()
{
	delete ui;
}

void CopyFilterGUI::configureChannels(vector<wstring>& channelNames)
{
	vector<Assignment> assignments = ui->form->buildAssignments();

	if(channelNames != inputChannelNames)
	{
		inputChannelNames = channelNames;

		scene->load(inputChannelNames, assignments);
		ui->form->setChannelNames(channelNames);
	}

	for(Assignment assignment : assignments)
	{
		if(assignment.targetChannel == L"")
			continue;
		bool hasSummand = false;
		for(Assignment::Summand summand : assignment.sourceSum)
		{
			if(summand.channel != L" ")
			{
				hasSummand = true;
				break;
			}
		}
		if(!hasSummand)
			continue;

		int channelIndex = ChannelHelper::getChannelIndex(assignment.targetChannel, channelNames, true);
		if(channelIndex == -1)
			channelNames.push_back(assignment.targetChannel);
	}
}

void CopyFilterGUI::store(QString& command, QString& parameters)
{
	command = "Copy";

	std::vector<Assignment> assignments;

	if(ui->tabWidget->currentIndex() == 0)
		assignments = scene->buildAssignments();
	else
		assignments = ui->form->buildAssignments();

	bool firstAssignment = true;
	for(const Assignment& assignment : assignments)
	{
		if(assignment.targetChannel == L"")
			continue;

		bool firstSummand = true;
		for(const Assignment::Summand& summand : assignment.sourceSum)
		{
			// skip not yet filled row
			if(summand.channel == L" ")
				continue;

			if(firstSummand)
			{
				firstSummand = false;

				if(firstAssignment)
					firstAssignment = false;
				else
					parameters += " ";

				parameters += QString::fromStdWString(assignment.targetChannel);
				parameters += "=";
			}
			else
			{
				parameters += "+";
			}

			bool hasChannel = summand.channel != L"";
			bool hasFactor = !hasChannel || summand.factor != 1.0 || summand.isDecibel;

			if(hasFactor)
			{
				QString factorString;
				factorString.setNum(summand.factor);
				if(factorString != "0" && !factorString.contains('.'))
					factorString += ".0";
				parameters += factorString;
				if(summand.isDecibel)
					parameters += "dB";
			}

			if(hasFactor && hasChannel)
				parameters += "*";

			if(hasChannel)
				parameters += QString::fromStdWString(summand.channel);
		}

		if(ui->tabWidget->currentIndex() == 0)
			ui->form->load(assignments);
		else
			scene->load(inputChannelNames, assignments);
	}
}