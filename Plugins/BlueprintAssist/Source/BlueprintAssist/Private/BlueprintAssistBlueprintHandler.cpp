// Copyright 2021 fpwong. All Rights Reserved.

#include "BlueprintAssistBlueprintHandler.h"

#include "BlueprintAssistGlobals.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"

const FString& FBAVariableDescription::GetMetaData(FName Key) const
{
	int32 EntryIndex = FindMetaDataEntryIndexForKey(Key);
	check(EntryIndex != INDEX_NONE);
	return MetaDataArray[EntryIndex].DataValue;
}

bool FBAVariableDescription::HasMetaData(FName Key) const
{
	return FindMetaDataEntryIndexForKey(Key) != INDEX_NONE;
}

int32 FBAVariableDescription::FindMetaDataEntryIndexForKey(FName Key) const
{
	for(int32 i=0; i<MetaDataArray.Num(); i++)
	{
		if(MetaDataArray[i].DataKey == Key)
		{
			return i;
		}
	}
	return INDEX_NONE;
}

FBABlueprintHandler::~FBABlueprintHandler()
{
	if (BlueprintPtr.IsValid())
	{
		BlueprintPtr->OnChanged().RemoveAll(this);
	}

	if (GEditor)
	{
		GEditor->OnObjectsReplaced().RemoveAll(this);
	}
}

void FBABlueprintHandler::BindBlueprintChanged(UBlueprint* Blueprint)
{
	BlueprintPtr = Blueprint;
	SetLastVariables(Blueprint);
	bProcessedChangesThisFrame = false;
	bActive = true;

	Blueprint->OnChanged().RemoveAll(this);
	Blueprint->OnChanged().AddRaw(this, &FBABlueprintHandler::OnBlueprintChanged);

	if (GEditor)
	{
		GEditor->OnObjectsReplaced().RemoveAll(this);
		GEditor->OnObjectsReplaced().AddRaw(this, &FBABlueprintHandler::OnObjectsReplaced);
	}
}

void FBABlueprintHandler::UnbindBlueprintChanged(UBlueprint* Blueprint)
{
	LastVariables.Empty();
	bProcessedChangesThisFrame = false;
	bActive = false;

	if (BlueprintPtr.IsValid() && BlueprintPtr->IsValidLowLevelFast())
	{
		BlueprintPtr->OnChanged().RemoveAll(this);
	}

	Blueprint->OnChanged().RemoveAll(this);
}

void FBABlueprintHandler::SetLastVariables(UBlueprint* Blueprint)
{
	LastVariables.Empty(Blueprint->NewVariables.Num());

	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		// Only copy the required values
		LastVariables.Add(FBAVariableDescription(Variable));
	}
}
// See UControlRigBlueprint::OnPostVariableChange
void FBABlueprintHandler::OnBlueprintChanged(UBlueprint* Blueprint)
{
	if (Blueprint != BlueprintPtr)
	{
		UE_LOG(LogBlueprintAssist, Warning, TEXT("Blueprint was changed but it's the wrong blueprint?"));
		return;
	}

	if (!bActive)
	{
		return;
	}

	if (bProcessedChangesThisFrame)
	{
		return;
	}

	bProcessedChangesThisFrame = true;
	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateRaw(this, &FBABlueprintHandler::ResetProcessedChangesThisFrame));

	if (Blueprint->IsPendingKill())
	{
		UE_LOG(LogBlueprintAssist, Warning, TEXT("Blueprint was changed while PendingKill, please report this on github!"));
		return;
	}
	
	// This shouldn't happen!
	check(Blueprint->IsValidLowLevelFast(false));

	TMap<FGuid, int32> OldVariablesByGuid;
	for (int32 VarIndex = 0; VarIndex < LastVariables.Num(); VarIndex++)
	{
		OldVariablesByGuid.Add(LastVariables[VarIndex].VarGuid, VarIndex);
	}

	for (FBPVariableDescription& BPNewVariable : Blueprint->NewVariables)
	{
		FBAVariableDescription NewVariable(BPNewVariable);
		if (!OldVariablesByGuid.Contains(NewVariable.VarGuid))
		{
			OnVariableAdded(Blueprint, NewVariable);
			continue;
		}

		const int32 OldVarIndex = OldVariablesByGuid.FindChecked(NewVariable.VarGuid);
		const FBAVariableDescription& OldVariable = LastVariables[OldVarIndex];

		// Make set instance editable to true when you set expose on spawn to true
		if (FBAUtils::HasMetaDataChanged(OldVariable, NewVariable, FBlueprintMetadata::MD_ExposeOnSpawn))
		{
			if (NewVariable.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) && NewVariable.GetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn) == TEXT("true"))
			{
				FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, NewVariable.VarName, false);
			}
		}

		// Check if a variable has been renamed (use string cause names are not case-sensitive!)
		if (!OldVariable.VarName.ToString().Equals(NewVariable.VarName.ToString()))
		{
			OnVariableRenamed(Blueprint, OldVariable, NewVariable);
		}

		// Check if a variable type has changed
		if (OldVariable.VarType != NewVariable.VarType)
		{
			OnVariableTypeChanged(Blueprint, OldVariable, NewVariable);
		}
	}

	SetLastVariables(Blueprint);
}

void FBABlueprintHandler::ResetProcessedChangesThisFrame()
{
	bProcessedChangesThisFrame = false;
}

void FBABlueprintHandler::OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap)
{
	if (BlueprintPtr.IsValid())
	{
		if (UObject* Replacement = ReplacementMap.FindRef(BlueprintPtr.Get()))
		{
			UE_LOG(LogBlueprintAssist, Warning, TEXT("Blueprint was replaced with %s"), *Replacement->GetName());
			UnbindBlueprintChanged(BlueprintPtr.Get());

			if (UBlueprint* NewBlueprint = Cast<UBlueprint>(Replacement))
			{
				BindBlueprintChanged(NewBlueprint);
			}
			else
			{
				BlueprintPtr = nullptr;
			}
		}
	}
}

void FBABlueprintHandler::OnVariableAdded(UBlueprint* Blueprint, FBAVariableDescription& Variable)
{
	const UBASettings* BASettings = GetDefault<UBASettings>();
	if (BASettings->bEnableVariableDefaults)
	{
		if (BASettings->bDefaultInstanceEditable)
		{
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, Variable.VarName, false);
		}

		if (BASettings->bDefaultBlueprintReadOnly)
		{
			FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, Variable.VarName, true);
		}

		if (BASettings->bDefaultExposeOnSpawn)
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true"));
		}

		if (BASettings->bDefaultPrivate)
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_Private, TEXT("true"));
		}

		if (BASettings->bDefaultExposeToCinematics)
		{
			FBlueprintEditorUtils::SetInterpFlag(Blueprint, Variable.VarName, true);
		}

		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, Variable.VarName, nullptr, BASettings->DefaultCategory);

		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Variable.VarName, nullptr, FBlueprintMetadata::MD_Tooltip, BASettings->DefaultTooltip.ToString());
	}
}

void FBABlueprintHandler::OnVariableRenamed(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable)
{
	if (GetDefault<UBASettings>()->bAutoRenameGettersAndSetters)
	{
		RenameGettersAndSetters(Blueprint, OldVariable, NewVariable);
	}
}

void FBABlueprintHandler::OnVariableTypeChanged(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable)
{
	// Boolean variables may need to be renamed as well!
	if (GetDefault<UBASettings>()->bAutoRenameGettersAndSetters)
	{
		RenameGettersAndSetters(Blueprint, OldVariable, NewVariable);
	}
}

void FBABlueprintHandler::RenameGettersAndSetters(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable)
{
	const FString OldVariableName = FBAUtils::GetVariableName(OldVariable.VarName.ToString(), OldVariable.VarType.PinCategory);
	const FString NewVariableName = FBAUtils::GetVariableName(NewVariable.VarName.ToString(), NewVariable.VarType.PinCategory);

	// Do nothing if our names didn't change
	if (OldVariableName == NewVariableName)
	{
		return;
	}

	const FString GetterName = FString::Printf(TEXT("Get%s"), *OldVariableName);
	const FString SetterName = FString::Printf(TEXT("Set%s"), *OldVariableName);

	const FString NewGetterName = FString::Printf(TEXT("Get%s"), *NewVariableName);
	const FString NewSetterName = FString::Printf(TEXT("Set%s"), *NewVariableName);

	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (FunctionGraph->GetName() == GetterName)
		{
			FBlueprintEditorUtils::RenameGraph(FunctionGraph, NewGetterName);
		}
		else if (FunctionGraph->GetName() == SetterName)
		{
			FBlueprintEditorUtils::RenameGraph(FunctionGraph, NewSetterName);
		}
	}
}
