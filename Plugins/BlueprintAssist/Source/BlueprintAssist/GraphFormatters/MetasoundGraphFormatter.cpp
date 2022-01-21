// Copyright 2021 fpwong. All Rights Reserved.

#include "MetasoundGraphFormatter.h"

FMetasoundGraphFormatter::FMetasoundGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler)
	: FSimpleFormatter(InGraphHandler)
{
	FormatterDirection = EGPD_Output;
}

FBAFormatterSettings FMetasoundGraphFormatter::GetFormatterSettings()
{
	return GetDefault<UBASettings>()->MetasoundGraphFormatterSettings;
}
