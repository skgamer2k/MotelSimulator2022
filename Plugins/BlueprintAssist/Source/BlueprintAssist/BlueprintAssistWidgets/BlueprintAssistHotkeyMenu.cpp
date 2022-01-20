// Copyright 2021 fpwong. All Rights Reserved.

#include "BlueprintAssistHotkeyMenu.h"

#include "BlueprintAssistInputProcessor.h"
#include "BlueprintAssistModule.h"
#include "Widgets/Layout/SScrollBox.h"

void SBAHotkeyMenu::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBAFilteredList<TSharedPtr<FBAHotkeyItem>>)
		.InitListItems(this, &SBAHotkeyMenu::InitListItems)
		.OnGenerateRow(this, &SBAHotkeyMenu::CreateItemWidget)
		// .OnSelectItem(this, &SBAHotkeyMenu::SelectItem)
		.WidgetSize(GetWidgetSize())
		.MenuTitle(FString("Blueprint Assist Hotkeys"))
	];
}

void SBAHotkeyMenu::InitListItems(TArray<TSharedPtr<FBAHotkeyItem>>& Items)
{
	const FInputBindingManager& InputBindingManager = FInputBindingManager::Get();

	TArray<TSharedPtr<FUICommandInfo>> CommandInfos;

	InputBindingManager.GetCommandInfosFromContext("BlueprintAssistCommands", CommandInfos);

	for (TSharedPtr<FUICommandInfo> Command : CommandInfos)
	{
		Items.Add(MakeShareable(new FBAHotkeyItem(Command)));
	}
}

TSharedRef<ITableRow> SBAHotkeyMenu::CreateItemWidget(TSharedPtr<FBAHotkeyItem> Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable).Padding(FMargin(8.0, 4.0))
	.ToolTipText(Item->CommandDesc)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
				[
					SNew(STextBlock).Text(FText::FromName(Item->CommandName))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SSpacer).Size(FVector2D(4.0f, 0.0f))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
				[
					SNew(STextBlock).Text(Item->CommandChord)
					.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().VAlign(VAlign_Bottom)
			[
				SNew(SScrollBox)
				.Orientation(Orient_Horizontal)
				+ SScrollBox::Slot()
				[
					SNew(STextBlock).Text(Item->CommandDesc)
					.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
					.ColorAndOpacity(FLinearColor::Gray)
				]
			]
		]
	];
}

void SBAHotkeyMenu::SelectItem(TSharedPtr<FBAHotkeyItem> Item)
{
	FBAInputProcessor::Get().TryExecuteCommand(Item->CommandInfo.ToSharedRef());
}
