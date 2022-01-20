// Copyright 2021 fpwong. All Rights Reserved.

#include "MaterialGraphFormatter.h"

FMaterialGraphFormatter::FMaterialGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler)
	: FSimpleFormatter(InGraphHandler)
{
	FormatterDirection = EGPD_Input;
}

FBAFormatterSettings FMaterialGraphFormatter::GetFormatterSettings()
{
	return GetDefault<UBASettings>()->MaterialGraphFormatterSettings;
}
