// Copyright 2021 fpwong. All Rights Reserved.

#include "BAFilteredList.h"

#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SSeparator.h"

template <typename ItemType>
SBAFilteredList<ItemType>::SBAFilteredList() {}

template <typename ItemType>
void SBAFilteredList<ItemType>::Construct(const FArguments& InArgs)
{
	OnSelectItem = InArgs._OnSelectItem;
	OnMarkActiveSuggestion = InArgs._OnMarkActiveSuggestion;
	WidgetSize = InArgs._WidgetSize;
	MenuTitle = InArgs._MenuTitle;
	SelectionMode = InArgs._SelectionMode;

	AllItems.Empty();

	InArgs._InitListItems.Execute(AllItems);

	FilteredItems = AllItems;

	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SBAFilteredList::SetFocusPostConstruct));

	SBorder::Construct(
		SBorder::FArguments()
#if ENGINE_MAJOR_VERSION >= 5
		.BorderImage(FEditorStyle::GetBrush("Brushes.Background"))
#else
		.BorderImage(FEditorStyle::GetBrush("ToolPanel.DarkGroupBorder"))
#endif
		.Padding(5)
		[
			SNew(SBox)
			.WidthOverride(WidgetSize.X)
			.HeightOverride(WidgetSize.Y)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(2)
				[

					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.VAlign(VAlign_Center)
					.HAlign(HAlign_Fill)
					.Padding(FMargin(8.f, 0.f))
					[
						SNew(SSeparator)
						.Orientation(Orient_Horizontal)
						.Thickness(1.0f)
						.SeparatorImage(FEditorStyle::GetBrush("Menu.Separator"))
					]
					+ SHorizontalBox::Slot()
					.VAlign(VAlign_Center)
					.HAlign(HAlign_Center)
					.Padding(FMargin(2.f, 0.f, 0.f, 0.f))
					.AutoWidth()
					[
						SNew(STextBlock)
#if ENGINE_MINOR_VERSION >= 26 || ENGINE_MAJOR_VERSION >= 5
						.TransformPolicy(ETextTransformPolicy::ToUpper)
#endif
						.Text(FText::FromString(MenuTitle))
						.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.BoldFont")))
						.TextStyle(FEditorStyle::Get(), TEXT("DetailsView.CategoryTextStyle"))
						.WrapTextAt(WidgetSize.X * 0.9f)
					]
					+ SHorizontalBox::Slot()
					.VAlign(VAlign_Center)
					.HAlign(HAlign_Fill)
					.Padding(FMargin(8.f, 0.f))
					[
						SNew(SSeparator)
						.Orientation(Orient_Horizontal)
						.Thickness(1.0f)
						.SeparatorImage(FEditorStyle::GetBrush("Menu.Separator"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
				[
					SAssignNew(FilterTextBox, SSearchBox)
					.OnTextChanged(this, &SBAFilteredList::OnFilterTextChanged)
					.OnTextCommitted(this, &SBAFilteredList::OnFilterTextCommitted)
					.OnKeyDownHandler(this, &SBAFilteredList::OnKeyDown)
				]
				+ SVerticalBox::Slot().FillHeight(1.f).Padding(2.f)
				[
					SNew(SBorder)
#if ENGINE_MAJOR_VERSION >= 5
					.BorderImage(FEditorStyle::GetBrush("Brushes.Panel"))
#else
					.BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
#endif
					[
						SAssignNew(FilteredItemsListView, SListView<ItemType>)
						.SelectionMode(SelectionMode)
						.ListItemsSource(&FilteredItems)
						.OnGenerateRow(InArgs._OnGenerateRow)
						.OnMouseButtonClick(this, &SBAFilteredList::OnListItemClicked)
						.IsFocusable(false)
					]
				]
			]
		]
	);
}

template <typename ItemType>
void SBAFilteredList<ItemType>::OnFilterTextChanged(const FText& InFilterText)
{
	FilterText = InFilterText;

	// Trim and sanitized the filter text (so that it more likely matches the action descriptions)
	const FString TrimmedFilterString = FText::TrimPrecedingAndTrailing(InFilterText).ToString();

	// Tokenize the search box text into a set of terms; all of them must be present to pass the filter
	TArray<FString> FilterTerms;
	TrimmedFilterString.ParseIntoArray(FilterTerms, TEXT(" "), true);

	FilteredItems.Empty();

	const bool bRequiresFiltering = FilterTerms.Num() > 0;
	for (int32 ItemIndex = 0; ItemIndex < AllItems.Num(); ++ItemIndex)
	{
		ItemType Item = AllItems[ItemIndex];

		// If we're filtering, search check to see if we need to show this action
		bool bShowAction = true;
		if (bRequiresFiltering)
		{
			const FString& SearchText = Item->GetSearchText();

			FString EachTermSanitized;
			for (int32 FilterIndex = 0; FilterIndex < FilterTerms.Num() && bShowAction; ++FilterIndex)
			{
				const bool bMatchesTerm = SearchText.Contains(FilterTerms[FilterIndex]);
				bShowAction = bShowAction && bMatchesTerm;
			}
		}

		if (bShowAction)
		{
			FilteredItems.Add(Item);
		}
	}

	if (!InFilterText.IsEmpty())
	{
		FilteredItems.StableSort([](const ItemType ItemA, const ItemType ItemB)
		{
			return ItemA->GetKeySearchText().Len() < ItemB->GetKeySearchText().Len();
		});
	}

	FilteredItemsListView->RequestListRefresh();

	// Make sure the selected suggestion stays within the filtered list
	if (SuggestionIndex >= 0 && FilteredItems.Num() > 0)
	{
		SuggestionIndex = FMath::Clamp<int32>(SuggestionIndex, 0, FilteredItems.Num() - 1);
		MarkActiveSuggestion();
	}
	else
	{
		SuggestionIndex = INDEX_NONE;
	}
}

template <typename ItemType>
void SBAFilteredList<ItemType>::OnFilterTextCommitted(const FText& InText, ETextCommit::Type CommitInfo)
{
	if (CommitInfo == ETextCommit::OnEnter)
	{
		SelectFirstItem();
	}
}

template <typename ItemType>
void SBAFilteredList<ItemType>::SelectItem(ItemType Item)
{
	FSlateApplication::Get().DismissMenuByWidget(SharedThis(this));
	OnSelectItem.ExecuteIfBound(Item);
}

template <typename ItemType>
bool SBAFilteredList<ItemType>::SelectFirstItem()
{
	if (FilteredItems.Num() > 0)
	{
		SelectItem(FilteredItems[0]);
		return true;
	}

	return false;
}

template <typename ItemType>
void SBAFilteredList<ItemType>::MarkActiveSuggestion()
{
	if (FilteredItems.IsValidIndex(SuggestionIndex))
	{
		ItemType& ItemToSelect = FilteredItems[SuggestionIndex];
		FilteredItemsListView->SetSelection(ItemToSelect);
		FilteredItemsListView->RequestScrollIntoView(ItemToSelect);
		OnMarkActiveSuggestion.ExecuteIfBound(ItemToSelect);
	}
	else
	{
		FilteredItemsListView->ClearSelection();
	}
}

template <typename ItemType>
FReply SBAFilteredList<ItemType>::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape)
	{
		FSlateApplication::Get().DismissMenuByWidget(SharedThis(this));
		return FReply::Handled();
	}

	if (KeyEvent.GetKey() == EKeys::Enter)
	{
		TArray<ItemType> SelectedItems;
		FilteredItemsListView->GetSelectedItems(SelectedItems);

		if (SelectedItems.Num() > 0)
		{
			SelectItem(SelectedItems[0]);
			return FReply::Handled();
		}

		if (SelectFirstItem())
		{
			return FReply::Handled();
		}
	}

	const int NumItems = FilteredItems.Num();
	if (NumItems > 0)
	{
		int32 SelectionDelta = 0;

		// move up and down through the filtered node list
		if (KeyEvent.GetKey() == EKeys::Up)
		{
			SelectionDelta = -1;
		}
		else if (KeyEvent.GetKey() == EKeys::Down)
		{
			SelectionDelta = 1;
		}

		if (SelectionDelta != 0)
		{
			// If we have no selected suggestion then we need to use the items in the root to set the selection and set the focus
			if (SuggestionIndex == INDEX_NONE)
			{
				SuggestionIndex = (SuggestionIndex + SelectionDelta + NumItems) % NumItems;
				MarkActiveSuggestion();
				return FReply::Handled();
			}

			//Move up or down one, wrapping around
			SuggestionIndex = (SuggestionIndex + SelectionDelta + NumItems) % NumItems;
			MarkActiveSuggestion();

			return FReply::Handled();
		}
	}

	return FReply::Unhandled();
}

template <typename ItemType>
EActiveTimerReturnType SBAFilteredList<ItemType>::SetFocusPostConstruct(double InCurrentTime, float InDeltaTime)
{
	if (FilterTextBox.IsValid())
	{
		FWidgetPath WidgetToFocusPath;
		FSlateApplication::Get().GeneratePathToWidgetUnchecked(FilterTextBox.ToSharedRef(), WidgetToFocusPath);
		FSlateApplication::Get().SetKeyboardFocus(WidgetToFocusPath, EFocusCause::SetDirectly);
		WidgetToFocusPath.GetWindow()->SetWidgetToFocusOnActivate(FilterTextBox);

		return EActiveTimerReturnType::Stop;
	}

	return EActiveTimerReturnType::Continue;
}

template <typename ItemType>
ItemType SBAFilteredList<ItemType>::GetSuggestedItem()
{
	return FilteredItems.IsValidIndex(SuggestionIndex) ? FilteredItems[SuggestionIndex] : nullptr;
}

template <typename ItemType>
FText SBAFilteredList<ItemType>::GetFilterText()
{
	return FilterText;
}
