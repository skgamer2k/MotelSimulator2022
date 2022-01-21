// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "BAFilteredList.h"

class SBlueprintContextTargetMenu;
class SWidget;

struct FBAHotkeyItem : IBAFilteredListItem
{
	FName CommandName;
	FText CommandChord;
	FText CommandDesc;
	TSharedPtr<const FUICommandInfo> CommandInfo;

	FBAHotkeyItem(TSharedPtr<FUICommandInfo> Command)
	{
		CommandInfo = Command;
		CommandName = Command->GetCommandName();
		CommandChord = Command->GetFirstValidChord()->GetInputText();

		if (CommandChord.IsEmptyOrWhitespace())
		{
			CommandChord = FText::FromString("Unbound");
		}

		CommandDesc = Command->GetDescription();
	};

	virtual FString ToString() const override { return CommandName.ToString(); }
};

class BLUEPRINTASSIST_API SBAHotkeyMenu : public SCompoundWidget
{
	// @formatter:off
	SLATE_BEGIN_ARGS(SBAHotkeyMenu) { }
	SLATE_END_ARGS()
	// @formatter:on

	static FVector2D GetWidgetSize() { return FVector2D(600, 500); }

	void Construct(const FArguments& InArgs);

	void InitListItems(TArray<TSharedPtr<FBAHotkeyItem>>& Items);

	TSharedRef<ITableRow> CreateItemWidget(TSharedPtr<FBAHotkeyItem> Item, const TSharedRef<STableViewBase>& OwnerTable) const;

	void SelectItem(TSharedPtr<FBAHotkeyItem> Item);
};
