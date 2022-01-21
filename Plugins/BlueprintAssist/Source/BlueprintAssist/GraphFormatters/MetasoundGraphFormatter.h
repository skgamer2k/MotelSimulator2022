// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "SimpleFormatter.h"

class BLUEPRINTASSIST_API FMetasoundGraphFormatter final
	: public FSimpleFormatter
{
public:
	FMetasoundGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler);
	virtual FBAFormatterSettings GetFormatterSettings() override;
};
