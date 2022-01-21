// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "BAFilteredList.h"
#include "BlueprintAssistTypes.h"
#include "PropertyPath.h"
#include "Widgets/SCompoundWidget.h"

class FBAInputProcessor;
class FBAGraphHandler;
class SBlueprintContextTargetMenu;
class SEditableTextBox;
class UEdGraph;
class UEdGraphNode;
class SWidget;

struct FBlueprintActionContext;
struct FCustomExpanderData;

class ITableRow;
class SGraphPanel;
class SSearchBox;
class UK2Node_Event;
class UBlueprint;

struct FEditDetailsStruct : IBAFilteredListItem
{
	TSharedPtr<SWidget> Widget;
	FString DetailName;
	FPropertyPath PropertyPath;

	FEditDetailsStruct(TSharedPtr<SWidget> InWidget, FString InDetailName)
		: Widget(InWidget)
		, DetailName(InDetailName) { }

	FEditDetailsStruct(FPropertyPath InPropertyPath)
	{
		SetPropertyPath(InPropertyPath);
	};

	void SetPropertyPath(const FPropertyPath& InPropertyPath)
	{
		PropertyPath = InPropertyPath;

		BA_WEAK_FIELD_PTR<BA_PROPERTY> Property = PropertyPath.GetRootProperty().Property;
		DetailName = Property.IsValid() ? Property->GetDisplayNameText().ToString() : PropertyPath.ToString();
	}

	virtual FString ToString() const override { return DetailName; }
};

class BLUEPRINTASSIST_API SEditDetailsMenu final : public SCompoundWidget
{
	// @formatter:off
	SLATE_BEGIN_ARGS(SEditDetailsMenu) { }
	SLATE_END_ARGS()
	// @formatter:on

	static FVector2D GetWidgetSize() { return FVector2D(400, 300); }

	void Construct(const FArguments& InArgs);

	void InitListItems(TArray<TSharedPtr<FEditDetailsStruct>>& Items);

	TSharedRef<ITableRow> CreateItemWidget(TSharedPtr<FEditDetailsStruct> Item, const TSharedRef<STableViewBase>& OwnerTable) const;

	void MarkActiveSuggestion(TSharedPtr<FEditDetailsStruct> Item);

	void SelectItem(TSharedPtr<FEditDetailsStruct> Item);
};
