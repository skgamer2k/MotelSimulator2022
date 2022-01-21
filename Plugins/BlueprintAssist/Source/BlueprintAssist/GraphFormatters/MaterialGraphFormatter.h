// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "SimpleFormatter.h"

class BLUEPRINTASSIST_API FMaterialGraphFormatter final
	: public FSimpleFormatter
{
public:
	FMaterialGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler);
	virtual FBAFormatterSettings GetFormatterSettings() override;
};
