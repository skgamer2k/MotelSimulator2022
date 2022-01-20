// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "SimpleFormatter.h"

class BLUEPRINTASSIST_API FControlRigGraphFormatter final
	: public FSimpleFormatter
{
public:
	FControlRigGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler);
	virtual FBAFormatterSettings GetFormatterSettings() override;
};
