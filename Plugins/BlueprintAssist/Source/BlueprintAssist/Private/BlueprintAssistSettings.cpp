// Copyright 2021 fpwong. All Rights Reserved.

#include "BlueprintAssistSettings.h"

#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistModule.h"
#include "BlueprintAssistSizeCache.h"
#include "BlueprintAssistTabHandler.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Interfaces/IPluginManager.h"
#include "Widgets/Input/SButton.h"

UBASettings::UBASettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisabledGraphs = { EBAGraphType::Unknown };

	UseBlueprintFormattingForTheseGraphs =
	{
		"EdGraph",
		"GameplayAbilityGraph",
		"AnimationTransitionGraph",
	};

	ExtraRootNodeTypes =
	{
		"AnimGraphNode_Root",
		"AnimGraphNode_TransitionResult",
		"AnimGraphNode_StateResult",
		"K2Node_Tunnel",
		"SoundCueGraphNode_Root",
		"BehaviorTreeGraphNode_Root",
		"MaterialGraphNode_Root",
		"NiagaraNodeInput",
		"MetasoundEditorGraphInputNode"
	};

	ShiftCameraDistance = 400.0f;

	bSaveBlueprintAssistCacheToFile = true;

	bAddToolbarWidget = true;

	PinHighlightColor = FLinearColor(0.2f, 0.2f, 0.2f);
	PinTextHighlightColor = FLinearColor(0.728f, 0.364f, 0.003f);

	// ------------------- //
	// Format all settings //
	// ------------------- //

	FormatAllStyle = EBAFormatAllStyle::Simple;
	bAutoPositionEventNodes = false;
	bAlwaysFormatAll = false;
	FormatAllPadding = FVector2D(800, 250);

	ExecutionWiringStyle = EBAWiringStyle::AlwaysMerge;
	ParameterWiringStyle = EBAWiringStyle::AlwaysMerge;

	FormattingStyle = EBANodeFormattingStyle::Expanded;
	ParameterStyle = EBAParameterFormattingStyle::Helixing;

	BlueprintParameterPadding = FVector2D(40, 25);
	BlueprintKnotTrackSpacing = 26.f;
	VerticalPinSpacing = 26.f;
	ParameterVerticalPinSpacing = 26.f;

	bLimitHelixingHeight = true;
	HelixingHeightMax = 500;
	SingleNodeMaxHeight = 300;

	// ------------------ //
	// Formatter Settings //
	// ------------------ //

	BlueprintFormatterSettings.Padding = FVector2D(80, 150);
	BlueprintFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	BehaviorTreeFormatterSettings.Padding = FVector2D(100, 100);
	BehaviorTreeFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	MaterialGraphFormatterSettings.Padding = FVector2D(200, 100);
	MaterialGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::Never;

	NiagaraGraphFormatterSettings.Padding = FVector2D(80, 150);
	NiagaraGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	AnimationGraphFormatterSettings.Padding = FVector2D(80, 150);
	AnimationGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	SoundCueGraphFormatterSettings.Padding = FVector2D(80, 150);
	SoundCueGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	ControlRigGraphFormatterSettings.Padding = FVector2D(80, 150);
	ControlRigGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	MetasoundGraphFormatterSettings.Padding = FVector2D(80, 150);
	MetasoundGraphFormatterSettings.AutoFormatting = EBAAutoFormatting::FormatAllConnected;

	bCreateKnotNodes = true;

	bBetterWiringForNewNodes = true;
	bAutoAddParentNode = true;

	bAutoRenameGettersAndSetters = true;
	bMergeGenerateGetterAndSetterButton = false;

	bSetAllCommentBubblePinned = false;

	bDetectNewNodesAndCacheNodeSizes = false;
	bRefreshNodeSizeBeforeFormatting = true;

	bTreatDelegatesAsExecutionPins = false;

	bCenterBranches = false;
	NumRequiredBranches = 3;

	bCenterBranchesForParameters = false;
	NumRequiredBranchesForParameters = 2;

	AutoInsertComment = EBAAutoInsertComment::Always;
	bTryToHandleCommentNodes = true;
	bAddKnotNodesToComments = true;
	CommentNodePadding = FVector2D(30, 30);

	bEnableFasterFormatting = false;

	bUseKnotNodePool = false;

	bSlowButAccurateSizeCaching = false;

	bAccountForCommentsWhenFormatting = false;

	KnotNodeDistanceThreshold = 800.f;

	bExpandNodesAheadOfParameters = true;
	bExpandNodesByHeight = true;
	bExpandParametersByHeight = false;

	bSnapToGrid = false;

	bEnableCachingNodeSizeNotification = true;
	RequiredNumPendingSizeForNotification = 50;

	// ------------------------ //
	// Create variable defaults //
	// ------------------------ //

	bEnableVariableDefaults = false;
	bDefaultInstanceEditable = false;
	bDefaultBlueprintReadOnly = false;
	bDefaultExposeOnSpawn = false;
	bDefaultPrivate = false;
	bDefaultExposeToCinematics = false;
	DefaultVariableName = TEXT("VarName");
	DefaultTooltip = FText::FromString(TEXT(""));
	DefaultCategory = FText::FromString(TEXT(""));

	// ------------------------ //
	// Misc                     //
	// ------------------------ //
	bPlayLiveCompileSound = false;

	bEnableInvisibleKnotNodes = false;

	bEnableShiftDraggingNodes = false;

	// ------------------------ //
	// Debug                    //
	// ------------------------ //
	bCustomDebug = -1;
}

void UBASettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	check(IBlueprintAssistModule::IsAvailable())

	const FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	TSharedPtr<FBAGraphHandler> GraphHandler = FBATabHandler::Get().GetActiveGraphHandler();
	if (GraphHandler.IsValid())
	{
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bSetAllCommentBubblePinned))
		{
			GraphHandler->ApplyCommentBubbleSetting();
		}
		else if (PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, ParameterStyle)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, FormattingStyle)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, ParameterWiringStyle)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, ExecutionWiringStyle)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bLimitHelixingHeight)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, HelixingHeightMax)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, SingleNodeMaxHeight)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, BlueprintKnotTrackSpacing)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, BlueprintParameterPadding)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, FormatAllPadding)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bTreatDelegatesAsExecutionPins)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bExpandNodesByHeight)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bExpandParametersByHeight)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UBASettings, bCreateKnotNodes)
			|| PropertyName == NAME_None) // if the name is none, this probably means we changed a property through the toolbar
			// TODO: maybe there's a way to change property externally while passing in correct info name
		{
			GraphHandler->ClearFormatters();
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

TSharedRef<IDetailCustomization> FBASettingsDetails::MakeInstance()
{
	return MakeShareable(new FBASettingsDetails);
}

void FBASettingsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);
	if (ObjectsBeingCustomized.Num() != 1)
	{
		return;
	}

	//--------------------
	// General
	// -------------------

	IDetailCategoryBuilder& GeneralCategory = DetailBuilder.EditCategory("General");
	auto& SizeCache = FBASizeCache::Get();

	const FString CachePath = SizeCache.GetCachePath();

	const auto DeleteSizeCache = [&SizeCache]()
	{
		SizeCache.DeleteCache();
		return FReply::Handled();
	};

	GeneralCategory.AddCustomRow(FText::FromString("Size Cache"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(FText::FromString("Size Cache"))
			.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
		]
		.ValueContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().Padding(5).AutoWidth()
			[
				SNew(SButton)
				.Text(FText::FromString("Delete size cache file"))
				.ToolTipText(FText::FromString(FString::Printf(TEXT("Delete size cache file located at: %s"), *CachePath)))
				.OnClicked_Lambda(DeleteSizeCache)
			]
		];
}
