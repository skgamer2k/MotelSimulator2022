// Copyright 2021 fpwong. All Rights Reserved.
#if 0

#include "BlueprintAssistCreateAssetMenu.h"

#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Editor/Kismet/Public/SSCSEditor.h"
#include "Runtime/SlateCore/Public/Types/SlateEnums.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STableViewBase.h"

/**********************/
/* FBACreateAssetItem */
/**********************/

class FContentBrowserModule;

FString FBACreateAssetItem::ToString() const
{
	return Factory->GetDisplayName().ToString();
}

void SBACreateAssetMenu::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBAFilteredList<TSharedPtr<FBACreateAssetItem>>)
		.InitListItems(this, &SBACreateAssetMenu::InitListItems)
		.OnGenerateRow(this, &SBACreateAssetMenu::CreateItemWidget)
		.OnSelectItem(this, &SBACreateAssetMenu::SelectItem)
		.WidgetSize(GetWidgetSize())
		.MenuTitle(FString("Create Asset"))
	];
}

void SBACreateAssetMenu::InitListItems(TArray<TSharedPtr<FBACreateAssetItem>>& Items)
{
	static const FName NAME_AssetTools = "AssetTools";
	const IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(NAME_AssetTools).Get();
	TArray<UFactory*> Factories = AssetTools.GetNewAssetFactories();
	for (UFactory* Factory : Factories)
	{
		Items.Add(MakeShareable(new FBACreateAssetItem(Factory)));
	}
}

TSharedRef<ITableRow> SBACreateAssetMenu::CreateItemWidget(TSharedPtr<FBACreateAssetItem> Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return
		SNew(STableRow<TSharedPtr<FString>>, OwnerTable).Padding(FMargin(4.0, 2.0))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().HAlign(HAlign_Left).FillWidth(1)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->ToString()))
			]
		];
}

void SBACreateAssetMenu::SelectItem(TSharedPtr<FBACreateAssetItem> Item)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	IContentBrowserSingleton& ContentBrowser = ContentBrowserModule.Get();
	const FString Path = ContentBrowser.GetCurrentPath();
	if (!Path.IsEmpty())
	{
		UFactory* Factory = Item->Factory;
		if (Factory)
		{
			if (Factory->ConfigureProperties())
			{
				FString DefaultAssetName;
				FString PackageNameToUse;

				static const FName NAME_AssetTools = "AssetTools";
				IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>(NAME_AssetTools).Get();
				AssetTools.CreateUniqueAssetName(Path / Factory->GetDefaultNewAssetName(), FString(), PackageNameToUse, DefaultAssetName);
				AssetTools.CreateAsset(DefaultAssetName, Path, Factory->SupportedClass, Factory);
			}
		}
	}
}

#endif