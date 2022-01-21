// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UEdGraphNode;

struct BLUEPRINTASSIST_API FFormatterInterface
	: public TSharedFromThis<FFormatterInterface>
{
	EEdGraphPinDirection FormatterDirection = EGPD_Output;

	virtual ~FFormatterInterface() = default;
	virtual void FormatNode(UEdGraphNode* Node) = 0;
	virtual TSet<UEdGraphNode*> GetFormattedNodes() = 0;
	virtual UEdGraphNode* GetRootNode() = 0;
};
