// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "SimpleFormatter.h"

class BLUEPRINTASSIST_API FSoundCueGraphFormatter final
	: public FSimpleFormatter
{
public:
	FSoundCueGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler);
	virtual FBAFormatterSettings GetFormatterSettings() override;
};
