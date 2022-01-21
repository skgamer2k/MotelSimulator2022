// Copyright 2021 fpwong. All Rights Reserved.

#include "EditDetailsMenu.h"

#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistUtils.h"
#include "BlueprintEditor.h"
#include "IDetailTreeNode.h"
#include "PropertyPath.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STableViewBase.h"

void SEditDetailsMenu::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBAFilteredList<TSharedPtr<FEditDetailsStruct>>)
		.InitListItems(this, &SEditDetailsMenu::InitListItems)
		.OnGenerateRow(this, &SEditDetailsMenu::CreateItemWidget)
		.OnSelectItem(this, &SEditDetailsMenu::SelectItem)
		.OnMarkActiveSuggestion(this, &SEditDetailsMenu::MarkActiveSuggestion)
		.WidgetSize(GetWidgetSize())
		.MenuTitle(FString("Edit Details"))
	];
}

void SEditDetailsMenu::InitListItems(TArray<TSharedPtr<FEditDetailsStruct>>& Items)
{
	TMap<FString, TSharedPtr<FEditDetailsStruct>> ItemsByName;

	// Grab items by widget
	TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();

	TArray<TSharedPtr<SWidget>> ItemRows;
	FBAUtils::GetChildWidgets(Window, "SDetailSingleItemRow", ItemRows);

	for (TSharedPtr<SWidget> Row : ItemRows)
	{
		TSharedPtr<SWidget> FoundWidget = FBAUtils::GetChildWidget(Row, "STextBlock");
		TSharedPtr<STextBlock> TextBlock = StaticCastSharedPtr<STextBlock>(FoundWidget);
		FString WidgetText = TextBlock->GetText().ToString();

		TSharedPtr<SWidget> FoundSplitter = FBAUtils::GetChildWidget(Row, "SSplitter");
		TSharedPtr<SSplitter> Splitter = StaticCastSharedPtr<SSplitter>(FoundSplitter);

		if (Splitter.IsValid())
		{
			FChildren* SplitterChildren = Splitter->GetChildren();
			if (SplitterChildren->Num() >= 2)
			{
				TSharedPtr<SWidget> WidgetToSearch = SplitterChildren->GetChildAt(1);
				TSharedPtr<SWidget> Interactable = FBAUtils::GetInteractableChildWidget(
					WidgetToSearch);

				if (Interactable.IsValid() && Interactable->GetParentWidget()->GetTypeAsString() !=
					"SResetToDefaultPropertyEditor")
				{
					ItemsByName.Add(WidgetText, MakeShareable(new FEditDetailsStruct(Interactable, WidgetText)));
				}
			}
		}
	}

	// Grab items by property from DetailView
	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	TSharedPtr<IDetailsView> DetailView = PropertyModule.FindDetailView("BlueprintInspector");
	if (DetailView)
	{
		for (FPropertyPath PropertyPath : DetailView->GetPropertiesInOrderDisplayed())
		{
			const FString PropertyDisplayString = PropertyPath.GetRootProperty().Property->GetDisplayNameText().ToString();
			if (TSharedPtr<FEditDetailsStruct>* FoundItem = ItemsByName.Find(PropertyDisplayString))
			{
				(*FoundItem)->SetPropertyPath(PropertyPath);
			}
			else
			{
				ItemsByName.Add(PropertyDisplayString, MakeShareable(new FEditDetailsStruct(PropertyPath)));
			}
		}
	}

	for (auto Elem : ItemsByName)
	{
		Items.Add(Elem.Value);
	}
}

TSharedRef<ITableRow> SEditDetailsMenu::CreateItemWidget(TSharedPtr<FEditDetailsStruct> Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return
		SNew(STableRow<TSharedPtr<FString>>, OwnerTable).Padding(FMargin(2.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Left).FillWidth(1)
			[
				SNew(STextBlock).Text(FText::FromString(Item->DetailName))
			]
		];
}

void SEditDetailsMenu::MarkActiveSuggestion(TSharedPtr<FEditDetailsStruct> Item)
{
	if (Item->PropertyPath.GetNumProperties() > 0)
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		TSharedPtr<IDetailsView> DetailView = PropertyModule.FindDetailView("BlueprintInspector");
		DetailView->HighlightProperty(Item->PropertyPath);
	}
}

void SEditDetailsMenu::SelectItem(TSharedPtr<FEditDetailsStruct> Item)
{
	if (Item->PropertyPath.GetNumProperties() > 0)
	{
		BA_WEAK_FIELD_PTR<BA_PROPERTY> Property = Item->PropertyPath.GetRootProperty().Property;
		if (Property.IsValid())
		{
			TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();

			TArray<TSharedPtr<SWidget>> ItemRows;
			FBAUtils::GetChildWidgets(Window, "SDetailSingleItemRow", ItemRows);

			for (TSharedPtr<SWidget> Row : ItemRows)
			{
				TSharedPtr<SWidget> FoundWidget = FBAUtils::GetChildWidget(Row, "STextBlock");
				TSharedPtr<STextBlock> TextBlock = StaticCastSharedPtr<STextBlock>(FoundWidget);
				FString WidgetText = TextBlock->GetText().ToString();

				if (WidgetText.Equals(Property->GetDisplayNameText().ToString()))
				{
					TSharedPtr<SWidget> FoundSplitter = FBAUtils::GetChildWidget(Row, "SSplitter");
					TSharedPtr<SSplitter> Splitter = StaticCastSharedPtr<SSplitter>(FoundSplitter);

					if (Splitter.IsValid())
					{
						FChildren* SplitterChildren = Splitter->GetChildren();
						if (SplitterChildren->Num() >= 2)
						{
							TSharedPtr<SWidget> WidgetToSearch = SplitterChildren->GetChildAt(1);
							TSharedPtr<SWidget> Interactable = FBAUtils::GetInteractableChildWidget(WidgetToSearch);

							if (Interactable.IsValid() && Interactable->GetParentWidget()->GetTypeAsString() != "SResetToDefaultPropertyEditor")
							{
								FBAUtils::InteractWithWidget(Interactable);
							}
						}
					}
				}
			}
		}
	}
	else // interact by saved widget
	{
		FBAUtils::InteractWithWidget(Item->Widget);
	}

	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	TSharedPtr<IDetailsView> DetailView = PropertyModule.FindDetailView("BlueprintInspector");
	DetailView->HighlightProperty(FPropertyPath());
}
