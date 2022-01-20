// Copyright 2021 fpwong. All Rights Reserved.

#include "ControlRigGraphFormatter.h"

FControlRigGraphFormatter::FControlRigGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler)
	: FSimpleFormatter(InGraphHandler)
{
	FormatterDirection = EGPD_Output;
}

FBAFormatterSettings FControlRigGraphFormatter::GetFormatterSettings()
{
	return GetDefault<UBASettings>()->ControlRigGraphFormatterSettings;
}
