// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

// TODO: make this a UObject so we don't need to create these structs
struct FBAVariableMetaDataEntry
{
	FName DataKey;
	FString DataValue;

	FBAVariableMetaDataEntry(const FBPVariableMetaDataEntry& MetaDataEntry)
		: DataKey(MetaDataEntry.DataKey)
		, DataValue(MetaDataEntry.DataValue) {}
};

struct FBAVariableDescription
{
	FName VarName;
	FGuid VarGuid;
	struct FEdGraphPinType VarType;

	TArray<FBAVariableMetaDataEntry> MetaDataArray;

	FBAVariableDescription(const FBPVariableDescription& VariableDescription)
	{
		VarName = VariableDescription.VarName;
		VarGuid = VariableDescription.VarGuid;
		VarType = VariableDescription.VarType;

		for (const auto& Entry : VariableDescription.MetaDataArray)
		{
			MetaDataArray.Add(FBAVariableMetaDataEntry(Entry));
		}
	}

	const FString& GetMetaData(FName Key) const;
	bool HasMetaData(FName Key) const;
	int32 FindMetaDataEntryIndexForKey(FName Key) const;
};

class FBABlueprintHandler
{
public:
	~FBABlueprintHandler();

	void BindBlueprintChanged(UBlueprint* Blueprint);

	void UnbindBlueprintChanged(UBlueprint* Blueprint);

	void SetLastVariables(UBlueprint* Blueprint);

	void OnBlueprintChanged(UBlueprint* Blueprint);

	void ResetProcessedChangesThisFrame();

	void OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);

	void OnVariableAdded(UBlueprint* Blueprint, FBAVariableDescription& Variable);

	void OnVariableRenamed(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable);

	void OnVariableTypeChanged(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable);

	void RenameGettersAndSetters(UBlueprint* Blueprint, const FBAVariableDescription& OldVariable, FBAVariableDescription& NewVariable);

private:
	TWeakObjectPtr<UBlueprint> BlueprintPtr;

	TArray<FBAVariableDescription> LastVariables;

	bool bProcessedChangesThisFrame = false;

	bool bActive = false;
};
