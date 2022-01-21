// Copyright 2021 fpwong. All Rights Reserved.

#include "NiagaraGraphFormatter.h"

FNiagaraGraphFormatter::FNiagaraGraphFormatter(const TSharedPtr<FBAGraphHandler>& InGraphHandler)
	: FSimpleFormatter(InGraphHandler)
{
	FormatterDirection = EGPD_Output;
}

FBAFormatterSettings FNiagaraGraphFormatter::GetFormatterSettings()
{
	return GetDefault<UBASettings>()->NiagaraGraphFormatterSettings;
}
