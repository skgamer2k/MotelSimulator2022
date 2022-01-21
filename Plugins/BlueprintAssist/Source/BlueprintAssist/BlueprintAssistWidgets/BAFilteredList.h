// Copyright 2021 fpwong. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IBAFilteredListItem
{
public:
	virtual ~IBAFilteredListItem() = default;

	virtual FString ToString() const = 0;

	virtual FString GetSearchText() const { return ToString(); }

	virtual FString GetKeySearchText() const { return ToString(); }
};

template <typename ItemType>
class BLUEPRINTASSIST_API SBAFilteredList
	: public SBorder
	, TListTypeTraits<ItemType>::SerializerType
{
	DECLARE_DELEGATE_RetVal_TwoParams(TSharedRef<class ITableRow>, FBAOnGenerateRow, ItemType, const TSharedRef<class STableViewBase>&);
	DECLARE_DELEGATE_OneParam(FBAInitListItems, TArray<ItemType>&);
	DECLARE_DELEGATE_OneParam(FBAOnSelectItem, ItemType);
	DECLARE_DELEGATE_OneParam(FBAOnMarkActiveSuggestion, ItemType);

public:
	// @formatter:off
	SLATE_BEGIN_ARGS(SBAFilteredList<ItemType>)
		: _WidgetSize(600, 500)
		, _MenuTitle("Menu Title")
		, _SelectionMode(ESelectionMode::Single) { }
		SLATE_EVENT(FBAInitListItems, InitListItems)
		SLATE_EVENT(FBAOnSelectItem, OnSelectItem)
		SLATE_EVENT(FBAOnMarkActiveSuggestion, OnMarkActiveSuggestion)
		SLATE_EVENT(FBAOnGenerateRow, OnGenerateRow)
		SLATE_ARGUMENT(FVector2D, WidgetSize)
		SLATE_ARGUMENT(FString, MenuTitle)
		SLATE_ARGUMENT(ESelectionMode::Type, SelectionMode)
	SLATE_END_ARGS()
	// @formatter:on

	SBAFilteredList();

	void Construct(const FArguments& InArgs);

	FVector2D WidgetSize;
	FString MenuTitle;
	ESelectionMode::Type SelectionMode;

	int32 SuggestionIndex = INDEX_NONE;
	TArray<ItemType> AllItems;
	TArray<ItemType> FilteredItems;
	TSharedPtr<SSearchBox> FilterTextBox;
	TSharedPtr<SListView<ItemType>> FilteredItemsListView;

	ItemType GetSuggestedItem();

	FText GetFilterText();

protected:
	virtual void OnFilterTextChanged(const FText& InFilterText);

	virtual void OnFilterTextCommitted(const FText& InText, ETextCommit::Type CommitInfo);

	virtual void OnListItemClicked(ItemType Item) { SelectItem(Item); }

	virtual void SelectItem(ItemType Item);

	virtual bool SelectFirstItem();

	virtual void MarkActiveSuggestion();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& KeyEvent) override;

private:
	FBAOnSelectItem OnSelectItem;
	FBAOnMarkActiveSuggestion OnMarkActiveSuggestion;
	FText FilterText;

	EActiveTimerReturnType SetFocusPostConstruct(double InCurrentTime, float InDeltaTime);
};
