// Copyright 2021 fpwong. All Rights Reserved.

#include "SoundCueGraphFormatter.h"

FSoundCueGraphFormatter::FSoundCueGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler)
	: FSimpleFormatter(InGraphHandler)
{
	FormatterDirection = EGPD_Input;
}

FBAFormatterSettings FSoundCueGraphFormatter::GetFormatterSettings()
{
	return GetDefault<UBASettings>()->SoundCueGraphFormatterSettings;
}
