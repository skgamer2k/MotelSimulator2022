// Copyright 2021 fpwong. All Rights Reserved.

#include "BlueprintAssistGraphHandler.h"

#include "BlueprintAssistGlobals.h"
#include "BlueprintAssistInputProcessor.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistSizeCache.h"
#include "BlueprintAssistUtils.h"
#include "BlueprintEditor.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "SCommentBubble.h"
#include "ScopedTransaction.h"
#include "SGraphPanel.h"
#include "BlueprintAssist/GraphFormatters/AnimationGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/BehaviorTreeGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/ControlRigGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/EdGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/MaterialGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/MetasoundGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/NiagaraGraphFormatter.h"
#include "BlueprintAssist/GraphFormatters/SoundCueGraphFormatter.h"
#include "Editor/BlueprintGraph/Classes/K2Node_Knot.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"

FBAGraphHandler::FBAGraphHandler(
	TWeakPtr<SDockTab> InTab,
	TWeakPtr<SGraphEditor> InGraphEditor)
	: CachedGraphEditor(InGraphEditor)
	, CachedTab(InTab)
{
	check(GetGraphEditor().IsValid());
	check(GetFocusedEdGraph() != nullptr);
	check(GetGraphPanel().IsValid());
	check(GetTab().IsValid());
	check(GetWindow().IsValid());

	InitGraphHandler();

	FCoreUObjectDelegates::OnObjectTransacted.AddRaw(this, &FBAGraphHandler::OnObjectTransacted);
}

FBAGraphHandler::~FBAGraphHandler()
{
	if (OnGraphChangedHandle.IsValid())
	{
		if (auto EdGraph = GetFocusedEdGraph())
		{
			EdGraph->RemoveOnGraphChangedHandler(OnGraphChangedHandle);
		}
	}

	FormatterMap.Empty();
	SelectedPinHandle = nullptr;
	FocusedNode = nullptr;
	LastSelectedNode = nullptr;
	LastNodes.Empty();
	ResetTransactions();

	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
}

void FBAGraphHandler::InitGraphHandler()
{
	Cleanup();

	DelayedGraphInitialized.StartDelay(2);
	DelayedGraphInitialized.SetOnDelayEnded(FBAOnDelayEnded::CreateRaw(this, &FBAGraphHandler::OnGraphInitializedDelayed));
	DelayedClearReplaceTransaction.SetOnDelayEnded(FBAOnDelayEnded::CreateRaw(this, &FBAGraphHandler::ResetReplaceNodeTransaction));
	DelayedDetectGraphChanges.SetOnDelayEnded(FBAOnDelayEnded::CreateRaw(this, &FBAGraphHandler::DetectGraphChanges));

	DelayedCacheSizeTimeout.SetOnDelayEnded(FBAOnDelayEnded::CreateRaw(this, &FBAGraphHandler::ShowSizeTimeoutNotification));

	NodeToReplace = nullptr;
	bInitialZoomFinished = false;
	NodeSizeTimeout = 0.f;
	FocusedNode = nullptr;
	bFullyZoomed = false;
	LastSelectedNode = nullptr;
	bLerpViewport = false;
	bCenterWhileLerping = false;

	SetSelectedPin(nullptr);

	FormatterParameters.Reset();
	PendingFormatting.Reset();
	PendingSize.Reset();
	CommentBubbleSizeCache.Reset();
	FormatAllColumns.Reset();
	FormatterMap.Reset();

	PendingTransaction.Reset();
	ReplaceNewNodeTransaction.Reset();
	FormatAllTransaction.Reset();

	CachedEdGraph.Reset();
	CachedEdGraph = GetFocusedEdGraph();

	GetGraphCache().CleanupGraph(GetFocusedEdGraph());

	GetGraphEditor()->GetViewLocation(LastGraphView, LastZoom);

	if (OnGraphChangedHandle.IsValid())
	{
		GetFocusedEdGraph()->RemoveOnGraphChangedHandler(OnGraphChangedHandle);
	}

	OnGraphChangedHandle = GetFocusedEdGraph()->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateRaw(this, &FBAGraphHandler::OnGraphChanged));
}

void FBAGraphHandler::OnGraphInitializedDelayed()
{
	LastNodes = GetFocusedEdGraph()->Nodes;

	if (GetDefault<UBASettings>()->bDetectNewNodesAndCacheNodeSizes)
	{
		CacheNodeSizes(GetFocusedEdGraph()->Nodes);
	}

	for (UEdGraphNode* Node : GetFocusedEdGraph()->Nodes)
	{
		NodeSizeChangeDataMap.Add(Node->NodeGuid, FBANodeSizeChangeData(Node));
	}
}

void FBAGraphHandler::OnGainFocus()
{
	if (NodeSizeTimeout > 0)
	{
		ShowSizeTimeoutNotification();
	}

	if (GetDefault<UBASettings>()->bEnableCachingNodeSizeNotification && PendingSize.Num() > GetDefault<UBASettings>()->RequiredNumPendingSizeForNotification)
	{
		ShowCachingNotification();
	}
}

void FBAGraphHandler::OnLoseFocus()
{
	if (CachingNotification.IsValid())
	{
		CachingNotification.Pin()->Fadeout();
	}

	if (SizeTimeoutNotification.IsValid())
	{
		SizeTimeoutNotification.Pin()->Fadeout();
	}
}

void FBAGraphHandler::Cleanup()
{
	if (OnGraphChangedHandle.IsValid())
	{
		if (auto EdGraph = GetFocusedEdGraph())
		{
			EdGraph->RemoveOnGraphChangedHandler(OnGraphChangedHandle);
		}
	}

	FormatterParameters.Reset();
	ResetTransactions();
	FormatterMap.Reset();
	NodeToReplace = nullptr;
	NodeSizeChangeDataMap.Reset();

	DelayedGraphInitialized.Cancel();
	DelayedViewportZoomIn.Cancel();
	DelayedClearReplaceTransaction.Cancel();
	DelayedDetectGraphChanges.Cancel();

	if (CachingNotification.IsValid())
	{
		CachingNotification.Pin()->ExpireAndFadeout();
	}

	if (SizeTimeoutNotification.IsValid())
	{
		SizeTimeoutNotification.Pin()->ExpireAndFadeout();
	}
}

void FBAGraphHandler::OnSelectionChanged(UEdGraphNode* PreviousNode, UEdGraphNode* NewNode)
{
	if (NewNode == nullptr)
	{
		SetSelectedPin(nullptr);
		return;
	}

	if (FBAUtils::IsCommentNode(NewNode) || FBAUtils::IsKnotNode(NewNode))
	{
		SetSelectedPin(nullptr);
		return;
	}

	TArray<UEdGraphPin*> Pins = FBAUtils::GetPinsByDirection(NewNode);
	UEdGraphPin* SelectedPin = GetSelectedPin();

	const bool bKeepCurrentPin = SelectedPin != nullptr && SelectedPin->GetOwningNode() == NewNode;
	if (bKeepCurrentPin)
	{
		return;
	}

	if (Pins.Num() > 0)
	{
		const auto& Sorter = [](const UEdGraphPin& PinA, const UEdGraphPin& PinB)
		{
			const int32 PinAExec = PinA.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			const int32 PinBExec = PinB.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;

			if (PinAExec != PinBExec)
			{
				return PinAExec > PinBExec;
			}

			return PinA.Direction > PinB.Direction;
		};

		Pins.StableSort(Sorter);

		SetSelectedPin(Pins[0]);
	}
	else
	{
		SetSelectedPin(nullptr);
	}
}

void FBAGraphHandler::LinkExecWhenCreatedFromParameter(UEdGraphNode* NodeCreated)
{
	if (!GetDefault<UBASettings>()->bBetterWiringForNewNodes)
	{
		return;
	}

	TArray<UEdGraphPin*> LinkedPins = FBAUtils::GetLinkedPins(NodeCreated);

	// if we drag off a parameter pin, link the exec pin too (if it exists)
	const auto IsPinOwningNodeImpure = [](UEdGraphPin* Pin)
	{
		return FBAUtils::IsNodeImpure(Pin->GetOwningNode());
	};

	const auto IsLinkedToImpureNode = [IsPinOwningNodeImpure](UEdGraphPin* Pin)
	{
		// skip delegate pins
		return !FBAUtils::IsDelegatePin(Pin) && Pin->LinkedTo.FilterByPredicate(IsPinOwningNodeImpure).Num() > 0;
	};

	TArray<UEdGraphPin*> PinsLinkedToImpureNodes = LinkedPins.FilterByPredicate(IsLinkedToImpureNode);

	if (PinsLinkedToImpureNodes.Num() == 1)
	{
		UEdGraphPin* MyLinkedPin = PinsLinkedToImpureNodes[0];
		if (MyLinkedPin->LinkedTo.Num() == 1)
		{
			UEdGraphPin* OtherLinkedPin = MyLinkedPin->LinkedTo[0];

			if (OtherLinkedPin != nullptr)
			{
				UEdGraphNode* OtherLinkedNode = OtherLinkedPin->GetOwningNode();

				if (FBAUtils::IsNodeImpure(OtherLinkedNode))
				{
					TArray<UEdGraphPin*> ExecPins = FBAUtils::GetExecPins(NodeCreated, MyLinkedPin->Direction);

					if (ExecPins.FilterByPredicate(FBAUtils::IsPinLinked).Num() == 0)
					{
						TArray<UEdGraphPin*> OtherExecPins = FBAUtils::GetExecPins(OtherLinkedNode, UEdGraphPin::GetComplementaryDirection(MyLinkedPin->Direction));

						UEdGraphPin* OtherExecPin = OtherExecPins[0];
						if (OtherExecPin->LinkedTo.Num() > 0)
						{
							TArray<UEdGraphPin*> MyPinsInDirection = FBAUtils::GetExecPins(NodeCreated, OtherExecPin->Direction);
							if (MyPinsInDirection.Num() > 0)
							{
								FBAUtils::TryCreateConnection(OtherExecPin->LinkedTo[0], MyPinsInDirection[0]);
							}
						}

						FBAUtils::TryCreateConnection(ExecPins[0], OtherExecPin);
						return;
					}
				}
			}
		}
	}
}

void FBAGraphHandler::AutoInsertExecNode(UEdGraphNode* NodeCreated)
{
	if (!GetDefault<UBASettings>()->bBetterWiringForNewNodes)
	{
		return;
	}

	// if we drag off an exec pin in the input direction creating node C in a chain say A->B
	// this code makes it so we create A->C->B (by default it create A->B | C<-B)
	TArray<UEdGraphPin*> LinkedToPins = FBAUtils::GetLinkedToPins(NodeCreated);
	if (LinkedToPins.FilterByPredicate(FBAUtils::IsExecPin).Num() == 1)
	{
		UEdGraphPin* PinOnB = LinkedToPins[0];
		if (PinOnB->Direction == EGPD_Output)
		{
			return;
		}

		TArray<UEdGraphPin*> NodeCreatedOutputExecPins = FBAUtils::GetExecPins(NodeCreated, EGPD_Input);
		if (NodeCreatedOutputExecPins.Num() > 0)
		{
			if (PinOnB->LinkedTo.Num() > 1)
			{
				UEdGraphPin* ExecPinOnA = nullptr;

				for (UEdGraphPin* Pin : PinOnB->LinkedTo)
				{
					if (Pin->GetOwningNode() != NodeCreated)
					{
						ExecPinOnA = Pin;
					}
				}

				if (ExecPinOnA != nullptr)
				{
					FBAUtils::TryCreateConnection(ExecPinOnA, NodeCreatedOutputExecPins[0]);
				}
			}
		}
	}
}

void FBAGraphHandler::AutoInsertParameterNode(UEdGraphNode* NodeCreated)
{
	if (!GetDefault<UBASettings>()->bBetterWiringForNewNodes)
	{
		return;
	}

	// if we drag off a pin creating node C in a chain A->B
	// this code makes it so we create A->C->B (by default it create A->B | A->C)
	TArray<UEdGraphPin*> LinkedParameterPins = FBAUtils::GetLinkedPins(NodeCreated).FilterByPredicate(FBAUtils::IsParameterPin);

	if (LinkedParameterPins.Num() > 0)
	{
		UEdGraphPin* MyLinkedPin = LinkedParameterPins[0];
		UEdGraphPin* OtherLinkedPin = MyLinkedPin->LinkedTo[0];

		UEdGraphPin* PinToLinkTo = nullptr;
		for (UEdGraphPin* Pin : OtherLinkedPin->LinkedTo)
		{
			if (Pin != MyLinkedPin)
			{
				PinToLinkTo = Pin;
				break;
			}
		}

		if (PinToLinkTo != nullptr)
		{
			// try to link one of our pins to the pin to link to
			for (UEdGraphPin* Pin : FBAUtils::GetParameterPins(NodeCreated, OtherLinkedPin->Direction))
			{
				if (Pin->PinType == PinToLinkTo->PinType)
				{
					bool bConnected = FBAUtils::TryCreateConnection(Pin, PinToLinkTo);
					if (bConnected)
					{
						return;
					}
				}
			}
		}
	}
}

void FBAGraphHandler::AutoInsertIntoCommentNodes(UEdGraphNode* NewNode)
{
	auto SelectedNodeCapture = LastSelectedNode;
	const auto IsSelectedNode = [SelectedNodeCapture](UEdGraphNode* LinkedNode) { return LinkedNode == SelectedNodeCapture; };
	auto LinkedInput = FBAUtils::GetLinkedNodes(NewNode, EGPD_Input).FilterByPredicate(IsSelectedNode);
	auto LinkedOutput = FBAUtils::GetLinkedNodes(NewNode, EGPD_Output).FilterByPredicate(IsSelectedNode);

	struct FLocal
	{
		static void TakeCommentNode(UEdGraph* Graph, UEdGraphNode* Node, UEdGraphNode* NodeToTakeFrom)
		{
			auto CommentNodes = FBAUtils::GetCommentNodesFromGraph(Graph);
			auto ContainingComments = FBAUtils::GetContainingCommentNodes(CommentNodes, NodeToTakeFrom);
			for (UEdGraphNode_Comment* CommentNode : ContainingComments)
			{
				CommentNode->AddNodeUnderComment(Node);
			}
		};
	};

	const auto AutoInsertStyle = GetDefault<UBASettings>()->AutoInsertComment;
	if (AutoInsertStyle == EBAAutoInsertComment::Surrounded)
	{
		if (LinkedInput.Num() == 1 && LinkedOutput.Num() == 1)
		{
			const auto CommentNodes = FBAUtils::GetCommentNodesFromGraph(GetFocusedEdGraph());
			auto ContainingCommentsA = FBAUtils::GetContainingCommentNodes(CommentNodes, LinkedOutput[0]);
			auto ContainingCommentsB = FBAUtils::GetContainingCommentNodes(CommentNodes, LinkedInput[0]);

			ContainingCommentsA.RemoveAll([&ContainingCommentsB](UEdGraphNode_Comment* Comment)
			{
				return !ContainingCommentsB.Contains(Comment);
			});

			if (ContainingCommentsA.Num() > 0)
			{
				FLocal::TakeCommentNode(GetFocusedEdGraph(), NewNode, ContainingCommentsA[0]);
			}
		}
	}
	else if (AutoInsertStyle == EBAAutoInsertComment::Always)
	{
		if (LinkedOutput.Num() == 1)
		{
			FLocal::TakeCommentNode(GetFocusedEdGraph(), NewNode, LinkedOutput[0]);
		}

		if (LinkedInput.Num() == 1)
		{
			FLocal::TakeCommentNode(GetFocusedEdGraph(), NewNode, LinkedInput[0]);
		}
	}
}

void FBAGraphHandler::Tick(float DeltaTime)
{
	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
	if (GraphPanel.IsValid() && CachedEdGraph != GraphPanel->GetGraphObj())
	{
		InitGraphHandler();
	}

	if (IsGraphReadOnly())
	{
		return;
	}

	if (DelayedGraphInitialized.IsComplete() && !bInitialZoomFinished)
	{
		if (LastGraphView == GraphPanel->GetViewOffset() && LastZoom == GraphPanel->GetZoomAmount())
		{
			bInitialZoomFinished = true;
		}

		GetGraphEditor()->GetViewLocation(LastGraphView, LastZoom);
	}

	DelayedGraphInitialized.Tick();

	DelayedDetectGraphChanges.Tick();

	UpdateCachedNodeSize(DeltaTime);

	UpdateSelectedNode();

	HighlightSelectedPin();

	UpdateNodesRequiringFormatting();

	UpdateLerpViewport(DeltaTime);
}

void FBAGraphHandler::UpdateSelectedNode()
{
	UEdGraphNode* CurrentSelectedNode = GetSelectedNode();

	if (CurrentSelectedNode != LastSelectedNode)
	{
		OnSelectionChanged(LastSelectedNode, CurrentSelectedNode);
	}

	LastSelectedNode = CurrentSelectedNode;
}

void FBAGraphHandler::HighlightSelectedPin()
{
	UEdGraphPin* SelectedPinObj = GetSelectedPin();
	if (SelectedPinObj == nullptr)
	{
		return;
	}

	if (FBAUtils::IsNodeDeleted(SelectedPinObj->GetOwningNode()))
	{
		return;
	}

	TSharedPtr<SGraphNode> GraphNode = GetGraphNode(SelectedPinObj->GetOwningNode());
	if (!GraphNode.IsValid())
	{
		return;
	}

	TSharedPtr<SGraphPin> GraphPin = FBAUtils::GetGraphPin(GetGraphPanel(), GetSelectedPin());
	if (GraphPin.IsValid())
	{
		GraphPin->SetPinColorModifier(GetMutableDefault<UBASettings>()->PinHighlightColor);
		GraphPin->SetColorAndOpacity(GetMutableDefault<UBASettings>()->PinTextHighlightColor);
	}
}

TSharedPtr<SWindow> FBAGraphHandler::GetWindow()
{
	check(CachedTab.IsValid())
	return FBAUtils::GetParentWindow(CachedTab.Pin());
}

bool FBAGraphHandler::IsWindowActive()
{
	return GetWindow() == FSlateApplication::Get().GetActiveTopLevelWindow();
}

bool FBAGraphHandler::IsGraphReadOnly()
{
	return FBlueprintEditorUtils::IsGraphReadOnly(GetFocusedEdGraph());
}

bool FBAGraphHandler::TryAutoFormatNode(UEdGraphNode* NewNodeToFormat, TSharedPtr<FScopedTransaction> InPendingTransaction, FEdGraphFormatterParameters Parameters)
{
	const auto AutoFormatting = FBAUtils::GetFormatterSettings(GetFocusedEdGraph()).AutoFormatting;

	if (NewNodeToFormat != nullptr && AutoFormatting != EBAAutoFormatting::Never)
	{
		// TODO: zoom to the newly created node
		if (GetDefault<UBASettings>()->bAutoPositionEventNodes && FBAUtils::IsEventNode(NewNodeToFormat))
		{
			FormatAllEvents();
		}
		else if (FBAUtils::GetLinkedNodes(NewNodeToFormat).Num() > 0)
		{
			if (AutoFormatting == EBAAutoFormatting::FormatSingleConnected)
			{
				Parameters.NodesToFormat = FBAUtils::GetLinkedNodes(NewNodeToFormat, EGPD_Input);
				if (Parameters.NodesToFormat.Num() == 0)
				{
					Parameters.NodesToFormat = FBAUtils::GetLinkedNodes(NewNodeToFormat, EGPD_Output);
				}
				Parameters.NodesToFormat.Add(NewNodeToFormat);
			}

			AddPendingFormatNodes(NewNodeToFormat, InPendingTransaction, Parameters);

			return true;
		}
	}

	return false;
}

void FBAGraphHandler::ResetGraphEditor(TWeakPtr<SGraphEditor> NewGraphEditor)
{
	CachedGraphEditor = NewGraphEditor;
	InitGraphHandler();
}

void FBAGraphHandler::ReplaceSavedSelectedNode(UEdGraphNode* NewNode)
{
	if (NodeToReplace != nullptr)
	{
		TArray<UEdGraphPin*> NodeToReplacePins = NodeToReplace->Pins;

		NodeToReplacePins.StableSort([](UEdGraphPin& PinA, UEdGraphPin& PinB)
		{
			return PinA.Direction > PinB.Direction;
		});

		TArray<FPinLink> PinsToLink;

		TArray<UEdGraphPin*> NewNodePins = NewNode->Pins;

		TSet<UEdGraphPin*> PinsConnected;

		// loop through our pins and check which pins can be connected to the new node
		for (int i = 0; i < 2; ++i)
		{
			for (UEdGraphPin* Pin : NodeToReplacePins)
			{
				if (Pin->LinkedTo.Num() == 0)
				{
					continue;
				}

				if (PinsConnected.Contains(Pin))
				{
					continue;
				}

				for (UEdGraphPin* NewNodePin : NewNodePins)
				{
					if (PinsConnected.Contains(NewNodePin))
					{
						continue;
					}

					// on the first run (i = 0), we only use pins which have the same name
					if (FBAUtils::GetPinName(Pin) == FBAUtils::GetPinName(NewNodePin) || i > 0)
					{
						TArray<UEdGraphPin*> LinkedTo = Pin->LinkedTo;

						bool bConnected = false;
						for (UEdGraphPin* LinkedPin : LinkedTo)
						{
							if (FBAUtils::CanConnectPins(LinkedPin, NewNodePin, true, false))
							{
								PinsToLink.Add(FPinLink(LinkedPin, NewNodePin));
								PinsConnected.Add(Pin);
								PinsConnected.Add(NewNodePin);
								bConnected = true;
							}
						}

						if (bConnected)
						{
							break;
						}
					}
				}
			}
		}

		// link the pins marked in the last two loops
		for (auto& PinToLink : PinsToLink)
		{
			for (UEdGraphPin* Pin : NewNode->Pins)
			{
				if (Pin->PinId == PinToLink.To->PinId)
				{
					FBAUtils::TryCreateConnection(PinToLink.From, Pin);
					break;
					//UE_LOG(LogBlueprintAssist, Warning, TEXT("\tConnected"));
				}
			}
		}

		// insert the new node into correct comment boxes
		const TArray<UEdGraphNode_Comment*> AllComments = FBAUtils::GetCommentNodesFromGraph(GetFocusedEdGraph());
		TArray<UEdGraphNode_Comment*> ContainingComments = FBAUtils::GetContainingCommentNodes(AllComments, NodeToReplace);
		for (UEdGraphNode_Comment* Comment : ContainingComments)
		{
			Comment->AddNodeUnderComment(NewNode);
		}

		FBAUtils::SafeDelete(AsShared(), NodeToReplace);

		NodeToReplace = nullptr;

		FEdGraphFormatterParameters Parameters;
		const bool bPendingFormatting = TryAutoFormatNode(NewNode);

		DelayedClearReplaceTransaction.Cancel();

		// when we format we will reset the transaction
		if (!bPendingFormatting)
		{
			ReplaceNewNodeTransaction.Reset();
		}
	}
}

void FBAGraphHandler::MoveUnrelatedNodes(TSharedPtr<FFormatterInterface> Formatter)
{
	// only move unrelated if we have an event node as our root node
	if (!FBAUtils::IsEventNode(Formatter->GetRootNode()))
	{
		return;
	}

	const TSet<UEdGraphNode*> FormattedNodes = Formatter->GetFormattedNodes();
	const FSlateRect FormatterBounds = FBAUtils::GetNodeArrayBounds(FormattedNodes.Array());

	UEdGraph* Graph = GetFocusedEdGraph();
	if (Graph == nullptr)
	{
		return;
	}

	float CHECK_INFINITE_LOOP = 0;

	// check all nodes on the graph
	TArray<UEdGraphNode*> Nodes = Graph->Nodes;

	while (Nodes.Num())
	{
		const auto NextNode = Nodes.Pop();

		if (FBAUtils::IsCommentNode(NextNode))
		{
			continue;
		}

		const auto NodeTree = FBAUtils::GetNodeTree(NextNode);

		const bool bSkipNodeTree = NodeTree.Array().ContainsByPredicate([&FormattedNodes](UEdGraphNode* Node)
		{
			return FormattedNodes.Contains(Node);
		});

		if (bSkipNodeTree)
		{
			continue;
		}

		const FSlateRect NodeTreeBounds = FBAUtils::GetNodeArrayBounds(NodeTree.Array());
		float OffsetX = 0;
		if (FSlateRect::DoRectanglesIntersect(FormatterBounds, NodeTreeBounds))
		{
			OffsetX = FormatterBounds.Bottom - NodeTreeBounds.Top + 20;
		}

		for (auto Node : NodeTree)
		{
			if (OffsetX != 0)
			{
				Node->Modify();
				Node->NodePosY += OffsetX;
			}

			Nodes.Remove(Node);
		}

		if (CHECK_INFINITE_LOOP++ > 10000)
		{
			UE_LOG(LogBlueprintAssist, Error, TEXT("Infinite loop detected in MoveUnrelatedNodes"));
			break;
		}
	}
}

void FBAGraphHandler::OnGraphChanged(const FEdGraphEditAction& Action)
{
	DelayedDetectGraphChanges.StartDelay(1);
}

void FBAGraphHandler::DetectGraphChanges()
{
	TArray<UEdGraphNode*> NewNodes;
	for (UEdGraphNode* NewNode : GetFocusedEdGraph()->Nodes)
	{
		if (FBAUtils::IsCommentNode(NewNode) || FBAUtils::IsKnotNode(NewNode))
		{
			continue;
		}

		if (!LastNodes.Contains(NewNode))
		{
			NewNodes.Add(NewNode);
		}
	}

	LastNodes = GetFocusedEdGraph()->Nodes;

	if (NewNodes.Num() > 0)
	{
		OnNodesAdded(NewNodes);
	}
}

void FBAGraphHandler::OnNodesAdded(const TArray<UEdGraphNode*>& NewNodes)
{
	for (UEdGraphNode* Node : NewNodes)
	{
		NodeSizeChangeDataMap.Add(Node->NodeGuid, FBANodeSizeChangeData(Node));
	}

	if (GetDefault<UBASettings>()->bDetectNewNodesAndCacheNodeSizes)
	{
		CacheNodeSizes(NewNodes);
	}

	if (NewNodes.Num() == 1)
	{
		UEdGraphNode* SingleNewNode = NewNodes[0];

		ReplaceSavedSelectedNode(SingleNewNode);

		// TODO: Test this logic on non-blueprint graphs
		if (FBAUtils::IsBlueprintGraph(GetFocusedEdGraph()))
		{
			if (FBAUtils::IsNodeImpure(SingleNewNode))
			{
				LinkExecWhenCreatedFromParameter(SingleNewNode);
				AutoInsertExecNode(SingleNewNode);
			}
			else if (FBAUtils::IsNodePure(SingleNewNode))
			{
				AutoInsertParameterNode(SingleNewNode);
			}

			AutoAddParentNode(SingleNewNode);
		}

		AutoInsertIntoCommentNodes(SingleNewNode);
	}

	FormatNewNodes(NewNodes);
}

void FBAGraphHandler::CacheNodeSizes(const TArray<UEdGraphNode*>& Nodes)
{
	for (UEdGraphNode* Node : Nodes)
	{
		if (FBAUtils::IsKnotNode(Node) || (!FBAUtils::IsGraphNode(Node) && !FBAUtils::IsCommentNode(Node)))
		{
			continue;
		}

		// if the node size hasn't been cached, add the node to be calculated
		if (!PendingSize.Contains(Node) && !GetGraphCache().CachedNodes.Contains(Node->NodeGuid))
		{
			PendingSize.Emplace(Node);
		}
	}
}

void FBAGraphHandler::FormatNewNodes(const TArray<UEdGraphNode*>& NewNodes)
{
	const auto AutoFormatting = FBAUtils::GetFormatterSettings(GetFocusedEdGraph()).AutoFormatting;
	if (AutoFormatting == EBAAutoFormatting::Never)
	{
		return;
	}

	// Check if we want to format all
	bool bHandledAlwaysFormatAll = false;
	if (GetDefault<UBASettings>()->bAlwaysFormatAll)
	{
		TArray<UEdGraphNode*> PendingNodes = NewNodes;
		int32 ErrorCount = 0;
		while (PendingNodes.Num() > 0)
		{
			ErrorCount += 1;
			if (ErrorCount > 1000)
			{
				UE_LOG(LogBlueprintAssist, Error, TEXT("BlueprintAssist: Error infinite loop detected in FBAGraphHandler::FormatNewNodes"));
				break;
			}

			UEdGraphNode* CurrentNode = PendingNodes.Pop();
			TArray<UEdGraphNode*> NodeTree = FBAUtils::GetNodeTree(CurrentNode).Array();

			auto FilterEvents = [](UEdGraphNode* Node)
			{
				return FBAUtils::IsEventNode(Node, EGPD_Output);
			};

			if (NodeTree.FilterByPredicate(FilterEvents).Num() > 0)
			{
				FormatAllEvents();
				bHandledAlwaysFormatAll = true;
				break;
			}

			PendingNodes.RemoveAllSwap([&NodeTree](UEdGraphNode* Node) { return NodeTree.Contains(Node); });
		}
	}

	if (bHandledAlwaysFormatAll)
	{
		return;
	}

	// if we are a new node and we are linked another node,
	// we were probably created from being dragged off a pin
	UEdGraphNode* NewNodeToFormat = nullptr;

	// if there is exactly 1 new node and it is linked, then format it

	FEdGraphFormatterParameters Parameters;

	if (NewNodes.Num() == 1)
	{
		NewNodeToFormat = NewNodes[0];

		const bool bIsParameterFormatter = !FBAUtils::GetNodeTree(NewNodeToFormat).Array().ContainsByPredicate(FBAUtils::IsNodeImpure);
		const EEdGraphPinDirection FormatterDirection = bIsParameterFormatter ? EGPD_Output : EGPD_Input;

		if (FBAUtils::GetLinkedPins(NewNodeToFormat, FormatterDirection).Num() == 0)
		{
			// node to keep still will be the pin we dragged off
			if (UEdGraphPin* SelectedPin = GetSelectedPin())
			{
				Parameters.NodeToKeepStill = SelectedPin->GetOwningNode();
			}
		}
	}
	else // multiple new nodes, check if there is exactly 1 impure node and use that
	{
		TArray<UEdGraphNode*> NewImpureNodes = NewNodes.FilterByPredicate(FBAUtils::IsNodeImpure);
		if (NewImpureNodes.Num() == 1)
		{
			NewNodeToFormat = NewImpureNodes[0];
		}
	}

	if (!NewNodeToFormat)
	{
		return;
	}
	
	TSharedPtr<FScopedTransaction> Transaction;
	if (!ReplaceNewNodeTransaction.IsValid() && !FormatAllTransaction.IsValid())
	{
		Transaction = MakeShareable(new FScopedTransaction(NSLOCTEXT("UnrealEd", "FormatNodeAfterAdding", "Format Node After Adding")));
	}

	TryAutoFormatNode(NewNodeToFormat, Transaction, Parameters);
}

void FBAGraphHandler::AutoAddParentNode(UEdGraphNode* NewNode)
{
	if (!GetDefault<UBASettings>()->bAutoAddParentNode)
	{
		return;
	}

	if (!FBAUtils::IsEventNode(NewNode))
	{
		return;
	}

	// See FBlueprintEditor::OnAddParentNode
	FFunctionFromNodeHelper FunctionFromNode(NewNode);
	if (FunctionFromNode.Function && FunctionFromNode.Node)
	{
		UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(GetFocusedEdGraph()->Schema);
		UFunction* ValidParent = Schema->GetCallableParentFunction(FunctionFromNode.Function);
		UEdGraph* TargetGraph = FunctionFromNode.Node->GetGraph();
		if (ValidParent && TargetGraph)
		{
			TSharedPtr<FScopedTransaction> Transaction = MakeShareable(new FScopedTransaction(FText::FromString("Auto-Add Parent Function Call")));
			TargetGraph->Modify();

			FGraphNodeCreator<UK2Node_CallParentFunction> FunctionNodeCreator(*TargetGraph);
			UK2Node_CallParentFunction* ParentFunctionNode = FunctionNodeCreator.CreateNode();
			ParentFunctionNode->SetFromFunction(ValidParent);
			ParentFunctionNode->AllocateDefaultPins();

			int32 NodeSizeY = 15;
			if (UK2Node* Node = Cast<UK2Node>(NewNode))
			{
				NodeSizeY += Node->DEPRECATED_NodeWidget.IsValid() ? static_cast<int32>(Node->DEPRECATED_NodeWidget.Pin()->GetDesiredSize().Y) : 0;
			}
			ParentFunctionNode->NodePosX = FunctionFromNode.Node->NodePosX;
			ParentFunctionNode->NodePosY = FunctionFromNode.Node->NodePosY + NodeSizeY;

			FunctionNodeCreator.Finalize();

			// The original event node may be linked, check linked to pins
			auto NodeLinkedToPins = FBAUtils::GetLinkedToPins(NewNode, EGPD_Output);
			for (auto OutputPin : FBAUtils::GetPinsByDirection(ParentFunctionNode, EGPD_Output))
			{
				for (auto Pin : NodeLinkedToPins)
				{
					if (FBAUtils::TryCreateConnection(OutputPin, Pin))
					{
						break;
					}
				}
			}

			// Link the original node to the parent
			for (auto OutputPin : FBAUtils::GetPinsByDirection(NewNode, EGPD_Output))
			{
				for (auto InputPin : FBAUtils::GetPinsByDirection(ParentFunctionNode, EGPD_Input))
				{
					if (FBAUtils::TryCreateConnection(OutputPin, InputPin))
					{
						break;
					}
				}
			}

			// We don't want to process the parent node as a new node, add it to last nodes so it will be ignored in the next check
			LastNodes.Add(ParentFunctionNode);
		}
	}
}

void FBAGraphHandler::ShowCachingNotification()
{
	if (CachingNotification.IsValid())
	{
		return;
	}

	FNotificationInfo Info(FText::GetEmpty());
	Info.ExpireDuration = 0.0f;
	Info.FadeInDuration = 0.0f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseSuccessFailIcons = true;
	Info.bUseThrobber = true;
	Info.bFireAndForget = false;
#if ENGINE_MAJOR_VERSION >= 5
	Info.ForWindow = GetWindow();
#endif
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		FText::FromString(TEXT("Cancel")),
		FText(),
		FSimpleDelegate::CreateRaw(this, &FBAGraphHandler::CancelCachingNotification),
		SNotificationItem::CS_Pending
	));

	CachingNotification = FSlateNotificationManager::Get().AddNotification(Info);
	CachingNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
	CachingNotification.Pin()->SetExpireDuration(0.0f);
	CachingNotification.Pin()->SetFadeOutDuration(0.5f);

	CachingNotification.Pin()->SetText(
		TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FBAGraphHandler::GetCachingMessage))
	);
}

void FBAGraphHandler::CancelCachingNotification()
{
	if (CachingNotification.IsValid())
	{
		CachingNotification.Pin()->SetExpireDuration(0.0f);
		CachingNotification.Pin()->SetFadeOutDuration(0.5f);
		CachingNotification.Pin()->ExpireAndFadeout();
		CachingNotification.Pin()->SetCompletionState(SNotificationItem::CS_Fail);
	}

	CancelProcessingNodeSizes();
}

void FBAGraphHandler::CancelFormattingNodes()
{
	PendingFormatting.Reset();
	PendingTransaction.Reset();
}

FText FBAGraphHandler::GetCachingMessage() const
{
	return FText::FromString(FString::Printf(TEXT("Caching nodes (%d)"), PendingSize.Num()));
}

void FBAGraphHandler::ShowSizeTimeoutNotification()
{
	if (SizeTimeoutNotification.IsValid())
	{
		return;
	}
	
	if (!FocusedNode)
	{
		return;
	}

	NodeSizeTimeout = 10.0f;

	FNotificationInfo Info(FText::GetEmpty());

	Info.ExpireDuration = 0.5f;
	Info.FadeInDuration = 0.1f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseSuccessFailIcons = true;
	Info.bFireAndForget = false;
#if ENGINE_MAJOR_VERSION >= 5
	Info.ForWindow = GetWindow();
#endif
	Info.Image = FEditorStyle::GetBrush(TEXT("Icons.Warning"));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		FText::FromString(TEXT("Cancel")),
		FText(),
		FSimpleDelegate::CreateRaw(this, &FBAGraphHandler::CancelSizeTimeoutNotification),
		SNotificationItem::CS_Pending
	));

	SizeTimeoutNotification = FSlateNotificationManager::Get().AddNotification(Info);
	SizeTimeoutNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);

	SizeTimeoutNotification.Pin()->SetText(
		TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FBAGraphHandler::GetSizeTimeoutMessage))
	);
}

void FBAGraphHandler::CancelSizeTimeoutNotification()
{
	if (SizeTimeoutNotification.IsValid())
	{
		SizeTimeoutNotification.Pin()->ExpireAndFadeout();

		const FString NotificationMsg = FString::Printf(
			TEXT("Warning: Node \"%s\" has failed to calculate size and is using the default size"),
			*FBAUtils::GetNodeName(FocusedNode));

		FNotificationInfo FailureInfo(FText::FromString(NotificationMsg));
		FailureInfo.ExpireDuration = 2.0f;
		FSlateNotificationManager::Get().AddNotification(FailureInfo)->SetCompletionState(SNotificationItem::CS_Fail);
	}

	NodeSizeTimeout = 0.0f;
}

FText FBAGraphHandler::GetSizeTimeoutMessage() const
{
	return FText::FromString(FString::Printf(
		TEXT("\"%s\" is not fully visible on screen. Please resize the window to fit the node. Timeout in %.0f..."),
		*FBAUtils::GetNodeName(FocusedNode),
		NodeSizeTimeout
	));
}

void FBAGraphHandler::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
	static const FName NodesChangedName(TEXT("Nodes"));

	if (Event.GetEventType() == ETransactionObjectEventType::UndoRedo)
	{
		if (Event.GetChangedProperties().Num() == 1 && Event.GetChangedProperties()[0].IsEqual(NodesChangedName))
		{
			if (UEdGraph* Graph = Cast<UEdGraph>(Object))
			{
				if (Graph == GetFocusedEdGraph())
				{
					LastNodes = GetFocusedEdGraph()->Nodes;
				}
			}
		}
	}
}

bool FBAGraphHandler::UpdateNodeSizesChanges(const TArray<UEdGraphNode*>& Nodes)
{
	bool bAddedSize = false;
	for (UEdGraphNode* Node : Nodes)
	{
		if (!FBAUtils::IsGraphNode(Node) || FBAUtils::IsKnotNode(Node))
		{
			continue;
		}

		// refresh node sizes for nodes which have changed in size 
		if (FBANodeSizeChangeData* ChangeData = NodeSizeChangeDataMap.Find(Node->NodeGuid))
		{
			if (ChangeData->HasNodeChanged(Node))
			{
				PendingSize.Add(Node);
				bAddedSize = true;
			}

			ChangeData->UpdateNode(Node);
		}
		else
		{
			NodeSizeChangeDataMap.Add(Node->NodeGuid, FBANodeSizeChangeData(Node));
		}

		// calculate size for all connected nodes which don't have a size
		if (!GetGraphCache().CachedNodes.Contains(Node->NodeGuid) && !PendingSize.Contains(Node))
		{
			PendingSize.Add(Node);
			bAddedSize = true;
		}
	}

	return bAddedSize;
}

UEdGraphNode* FBAGraphHandler::GetRootNode(UEdGraphNode* InitialNode, const TArray<UEdGraphNode*>& NodesToFormat, bool bCheckSelectedNode)
{
	auto Formatter = MakeFormatter();
	if (!Formatter.IsValid())
	{
		return nullptr;
	}

	EEdGraphPinDirection FormatterDirection = Formatter->FormatterDirection;

	const auto OppositeDirection = UEdGraphPin::GetComplementaryDirection(FormatterDirection);

	const auto NodeTreeFilter = [this, &NodesToFormat](const FPinLink& Link) { return FilterDelegatePin(Link, NodesToFormat); };
	TSet<UEdGraphNode*> NodeTree = FBAUtils::GetNodeTreeWithFilter(InitialNode, NodeTreeFilter);

	const bool bIsParameterTree = !NodeTree.Array().ContainsByPredicate(FBAUtils::IsNodeImpure);
	if (bIsParameterTree)
	{
		const auto Filter = [&](UEdGraphNode* Node)
		{
			return FBAUtils::IsNodePure(Node) && FilterSelectiveFormatting(Node, FormatterParameters.NodesToFormat);
		};

		// get the right-most pure node
		return FBAUtils::GetTopMostWithFilter(InitialNode, EGPD_Output, Filter);
	}

	TArray<UEdGraphNode*> EventNodes;
	TArray<UEdGraphNode*> UnlinkedNodes;
	TArray<UEdGraphNode*> RootNodes;

	for (UEdGraphNode* Node : NodeTree)
	{
		// UE_LOG(LogTemp, Warning, TEXT("Checking Node %s"), *FBAUtils::GetNodeName(Node));
		if (FBAUtils::IsKnotNode(Node))
		{
			continue;
		}

		if (FBAUtils::IsExtraRootNode(Node) && FBAUtils::DoesNodeHaveExecutionTo(InitialNode, Node))
		{
			// UE_LOG(LogTemp, Warning, TEXT("\tRoot node EXTRA %s"), *FBAUtils::GetNodeName(Node));
			RootNodes.Add(Node);
			continue;
		}

		if (FBAUtils::IsNodeImpure(Node))
		{
			if (FBAUtils::IsEventNode(Node, FormatterDirection) && FBAUtils::DoesNodeHaveExecutionTo(InitialNode, Node))
			{
				// UE_LOG(LogTemp, Warning, TEXT("\tRoot node EVENT %s"), *FBAUtils::GetNodeName(Node));
				EventNodes.Add(Node);
				continue;
			}

			TArray<UEdGraphPin*> LinkedInputPins = FBAUtils::GetLinkedPins(Node, OppositeDirection).FilterByPredicate(FBAUtils::IsExecPin);

			if (LinkedInputPins.Num() == 0 && FBAUtils::DoesNodeHaveExecutionTo(InitialNode, Node))
			{
				// UE_LOG(LogTemp, Warning, TEXT("\tRoot node UNLINKED %s"), *FBAUtils::GetNodeName(Node));
				UnlinkedNodes.Emplace(Node);
			}
		}
	}

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Events %d Unlinked %d Root %d"), EventNodes.Num(), UnlinkedNodes.Num(), RootNodes.Num());

	if (EventNodes.Num() == 0 && UnlinkedNodes.Num() == 0 && RootNodes.Num() == 0)
	{
		const auto Filter = [&](UEdGraphNode* Node) { return FilterSelectiveFormatting(Node, NodesToFormat) && FBAUtils::IsNodeImpure(Node); };
		UEdGraphNode* NodeInDirection = FBAUtils::GetTopMostWithFilter(InitialNode, OppositeDirection, Filter);

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Node in dir %s Dir %d"), *FBAUtils::GetNodeName(NodeInDirection), OppositeDirection);

		const TArray<UEdGraphNode*> Visited = { NodeInDirection };
		while (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(NodeInDirection))
		{
			const auto& LinkedOut = Knot->GetOutputPin()->LinkedTo;
			if (LinkedOut.Num() > 0)
			{
				auto NextNode = LinkedOut[0]->GetOwningNode();
				if (Visited.Contains(NextNode))
				{
					break;
				}

				NodeInDirection = NextNode;
			}
		}

		return NodeInDirection;
	}

	const auto& SortByDirection = [&FormatterDirection](const UEdGraphNode& A, const UEdGraphNode& B)
	{
		if (FormatterDirection == EGPD_Output) // sort left to right
		{
			if (A.NodePosX != B.NodePosX)
			{
				return A.NodePosX < B.NodePosX;
			}
		}
		else // sort right to left
		{
			if (A.NodePosX != B.NodePosX)
			{
				return A.NodePosX > B.NodePosX;
			}
		}

		// sort top to bottom
		return A.NodePosY < B.NodePosY;
	};

	UEdGraphNode* SelectedNode = GetSelectedNode();

	if (RootNodes.Num() > 0)
	{
		if (bCheckSelectedNode && RootNodes.Contains(SelectedNode))
		{
			return SelectedNode;
		}
		
		if (RootNodes.Contains(InitialNode))
		{
			return InitialNode;
		}

		RootNodes.StableSort(SortByDirection);
		RootNodes.StableSort([](UEdGraphNode& NodeA, UEdGraphNode& NodeB)
		{
			return FBAUtils::GetPinsByDirection(&NodeA, EGPD_Input).Num() < FBAUtils::GetPinsByDirection(&NodeB, EGPD_Input).Num();
		});

		return RootNodes[0];
	}

	if (EventNodes.Num() > 0) // use the top left most event node
	{
		if (bCheckSelectedNode && EventNodes.Contains(SelectedNode))
		{
			return SelectedNode;
		}

		if (EventNodes.Contains(InitialNode))
		{
			return InitialNode;
		}

		EventNodes.Sort(SortByDirection);
		return EventNodes[0];
	}

	if (bCheckSelectedNode && UnlinkedNodes.Contains(SelectedNode))
	{
		return SelectedNode;
	}
	
	if (UnlinkedNodes.Contains(InitialNode))
	{
		return InitialNode;
	}

	// use the top left most unlinked node
	UnlinkedNodes.Sort(SortByDirection);
	return UnlinkedNodes[0];
}

TSharedPtr<FFormatterInterface> FBAGraphHandler::MakeFormatter()
{
	UEdGraph* EdGraph = GetFocusedEdGraph();
	if (!EdGraph)
	{
		return nullptr;
	}

	EBAGraphType GraphType = FBAUtils::GetBAGraphType(EdGraph);

	switch (GraphType)
	{
		case EBAGraphType::Blueprint:
			return MakeShared<FEdGraphFormatter>(AsShared(), FormatterParameters);
		case EBAGraphType::BehaviorTree:
			return MakeShared<FBehaviorTreeGraphFormatter>(AsShared());
		case EBAGraphType::MaterialGraph:
			return MakeShared<FMaterialGraphFormatter>(AsShared());
		case EBAGraphType::NiagaraGraph:
			return MakeShared<FNiagaraGraphFormatter>(AsShared());
		case EBAGraphType::AnimGraph:
			return MakeShared<FAnimationGraphFormatter>(AsShared());
		case EBAGraphType::SoundCue:
			return MakeShared<FSoundCueGraphFormatter>(AsShared());
		case EBAGraphType::ControlRigGraph:
			return MakeShared<FControlRigGraphFormatter>(AsShared());
		case EBAGraphType::MetasoundGraph:
			return MakeShared<FMetasoundGraphFormatter>(AsShared());
		default:
		{
			const FName GraphClassName = EdGraph->GetClass()->GetFName();
			const FString GraphName = GraphClassName.ToString();
			UE_LOG(LogBlueprintAssist, Log,
				TEXT("Formatting for graph class %s not supported. Enable formatting by adding %s to the 'UseBlueprintFormattingForTheseGraphs' list in the settings"), *GraphName, *GraphName);
			return nullptr;
		}
	}
}

bool FBAGraphHandler::HasActiveTransaction() const
{
	return PendingTransaction.IsValid() && PendingTransaction->IsOutstanding() ||
		ReplaceNewNodeTransaction.IsValid() && ReplaceNewNodeTransaction->IsOutstanding() ||
		FormatAllTransaction.IsValid() && FormatAllTransaction->IsOutstanding();
}

bool FBAGraphHandler::FilterSelectiveFormatting(UEdGraphNode* Node, const TArray<UEdGraphNode*>& NodesToFormat)
{
	if (NodesToFormat.Num() > 0)
	{
		return NodesToFormat.Contains(Node);
	}

	return true;
}

bool FBAGraphHandler::FilterDelegatePin(const FPinLink& PinLink, const TArray<UEdGraphNode*>& NodesToFormat)
{
	if (!FilterSelectiveFormatting(PinLink.To->GetOwningNode(), NodesToFormat))
	{
		return false;
	}

	if (GetMutableDefault<UBASettings>()->bTreatDelegatesAsExecutionPins || !FBAUtils::IsDelegatePin(PinLink.From))
	{
		return true;
	}

	return FBAUtils::IsNodePure(PinLink.From->GetOwningNode()) || FBAUtils::IsNodePure(PinLink.To->GetOwningNode());
}

FBACacheData& FBAGraphHandler::GetGraphCache()
{
	return FBASizeCache::Get().GetGraphData(GetFocusedEdGraph());
}

UEdGraph* FBAGraphHandler::GetFocusedEdGraph()
{
	if (CachedEdGraph.IsValid())
	{
		return CachedEdGraph.Get();
	}

	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
	if (GraphPanel.IsValid())
	{
		return GraphPanel->GetGraphObj();
	}
	return nullptr;
}

TSharedPtr<SGraphEditor> FBAGraphHandler::GetGraphEditor()
{
	if (CachedGraphEditor.IsValid())
	{
		return CachedGraphEditor.Pin();
	}

	if (CachedTab.IsValid())
	{
		// grab the graph editor from the tab
		const TSharedRef<SWidget> TabContent = CachedTab.Pin()->GetContent();

		if (TabContent->GetTypeAsString() == "SGraphEditor")
		{
			TSharedPtr<SGraphEditor> TabContentAsGraphEditor = StaticCastSharedRef<SGraphEditor>(TabContent);

			if (TabContentAsGraphEditor.IsValid())
			{
				if (CachedGraphEditor != TabContentAsGraphEditor)
				{
					ResetGraphEditor(TWeakPtr<SGraphEditor>(TabContentAsGraphEditor));
					return CachedGraphEditor.Pin();
				}
			}
		}
	}

	return nullptr;
}

TSharedPtr<SGraphPanel> FBAGraphHandler::GetGraphPanel()
{
	if (CachedGraphPanel.IsValid())
	{
		return CachedGraphPanel.Pin();
	}

	TSharedPtr<SGraphEditor> GraphEditor = GetGraphEditor();
	if (!GraphEditor.IsValid())
	{
		return nullptr;
	}

	// try to grab the graph panel from the graph editor
	TSharedPtr<SWidget> GraphPanelWidget = FBAUtils::GetChildWidget(GraphEditor, "SGraphPanel");
	if (GraphPanelWidget.IsValid())
	{
		CachedGraphPanel = StaticCastSharedPtr<SGraphPanel>(GraphPanelWidget);
		return CachedGraphPanel.Pin();
	}

	return nullptr;
}

FSlateRect FBAGraphHandler::GetCachedNodeBounds(UEdGraphNode* Node, bool bWithCommentBubble)
{
	if (!Node)
	{
		return FSlateRect();
	}

	FVector2D Pos(Node->NodePosX, Node->NodePosY);

	FVector2D Size(300, 150);
	if (FBANodeData* FoundNodeData = GetGraphCache().CachedNodes.Find(Node->NodeGuid))
	{
		Size.X = FoundNodeData->CachedNodeSize.X;
		Size.Y = FoundNodeData->CachedNodeSize.Y;
	}

	FVector2D* CommentBubbleSizePtr = CommentBubbleSizeCache.Find(Node);
	if (bWithCommentBubble && CommentBubbleSizePtr != nullptr)
	{
		const FVector2D CommentBubbleSize = *CommentBubbleSizePtr;
		Pos.Y -= CommentBubbleSize.Y;
		Size.Y += CommentBubbleSize.Y;
	}

	return FSlateRect::FromPointAndExtent(Pos, Size);
}

UEdGraphPin* FBAGraphHandler::GetSelectedPin()
{
	if (!SelectedPinHandle.IsSet())
	{
		return nullptr;
	}

	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
	if (!GraphPanel.IsValid())
	{
		return nullptr;
	}

	UEdGraphPin* PinObj = SelectedPinHandle->GetPinObj(*GraphPanel.Get());
	if (!PinObj || PinObj->bHidden || PinObj->bWasTrashed || PinObj->bOrphanedPin)
	{
		return nullptr;
	}

	return PinObj;
}

UEdGraphNode* FBAGraphHandler::GetSelectedNode(bool bAllowCommentNodes)
{
	TArray<UEdGraphNode*> SelectedNodes = GetSelectedNodes(bAllowCommentNodes).Array();
	return SelectedNodes.Num() == 1 ? SelectedNodes[0] : nullptr;
}

TSet<UEdGraphNode*> FBAGraphHandler::GetSelectedNodes(bool bAllowCommentNodes)
{
	TSet<UEdGraphNode*> SelectedNodes;

	auto GraphEditor = GetGraphEditor();
	if (GraphEditor.IsValid())
	{
		for (UObject* Obj : GraphEditor->GetSelectedNodes())
		{
			if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
			{
				if (FBAUtils::IsGraphNode(Node) || FBAUtils::IsKnotNode(Node) || (bAllowCommentNodes && FBAUtils::IsCommentNode(Node)))
				{
					SelectedNodes.Emplace(Node);
				}
			}
		}
	}

	return SelectedNodes;
}

UBlueprint* FBAGraphHandler::GetBlueprint()
{
	if (UEdGraph* Graph = GetFocusedEdGraph())
	{
		return Graph->GetTypedOuter<UBlueprint>();
	}

	return nullptr;
}

void FBAGraphHandler::AddPendingFormatNodes(UEdGraphNode* Node, TSharedPtr<FScopedTransaction> InPendingTransaction, FEdGraphFormatterParameters InFormatterParameters)
{
	// UEditorEngine* Editor = static_cast<UEditorEngine*>(GEngine);
	// if (Editor->IsTransactionActive())
	// {
	// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("Cannot format while transaction is active"));
	// 	return;
	// }

	if (FBAUtils::IsGraphNode(Node))
	{
		PendingTransaction = InPendingTransaction;
		FormatterParameters = InFormatterParameters;
		PendingFormatting.Add(Node);
	}

	if (GetDefault<UBASettings>()->bRefreshNodeSizeBeforeFormatting)
	{
		TSet<UEdGraphNode*> NodeTree = FBAUtils::GetNodeTree(Node);
		UpdateNodeSizesChanges(NodeTree.Array());
	}
}

void FBAGraphHandler::ResetSingleNewNodeTransaction()
{
	DelayedClearReplaceTransaction.StartDelay(2);
}

void FBAGraphHandler::ResetReplaceNodeTransaction()
{
	if (ReplaceNewNodeTransaction.IsValid())
	{
		ReplaceNewNodeTransaction->Cancel();
		ReplaceNewNodeTransaction.Reset();
	}
}

float FBAGraphHandler::GetPinY(UEdGraphPin* Pin)
{
	if (!Pin) { return 0; }

	UEdGraphNode* OwningNode = Pin->GetOwningNode();
	if (!OwningNode)
	{
		UE_LOG(LogBlueprintAssist, Error, TEXT("GraphHandler: No owning node for pin %s"), *FBAUtils::GetPinName(Pin));
		return 0;
	}

	if (FBANodeData* FoundNodeData = GetGraphCache().CachedNodes.Find(OwningNode->NodeGuid))
	{
		if (float* FoundPinOffset = FoundNodeData->CachedPins.Find(Pin->PinId))
		{
			return Pin->GetOwningNode()->NodePosY + *FoundPinOffset;
		}
	}

	// cache pin offset
	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
	if (GraphPanel.IsValid())
	{
		TSharedPtr<SGraphNode> GraphNode = GetGraphNode(Pin->GetOwningNode());
		if (GraphNode.IsValid())
		{
			TSharedPtr<SGraphPin> GraphPin = GraphNode->FindWidgetForPin(Pin);
			if (GraphPin.IsValid())
			{
				if (GraphPin->GetPinObj() != nullptr)
				{
					return Pin->GetOwningNode()->NodePosY + GraphPin->GetNodeOffset().Y;
				}
			}
		}
	}

	return Pin->GetOwningNode()->NodePosY;
}

void FBAGraphHandler::UpdateCachedNodeSize(float DeltaTime)
{
	if (!bInitialZoomFinished)
	{
		return;
	}

	TSharedPtr<SGraphEditor> GraphEditor = GetGraphEditor();
	if (!GraphEditor.IsValid())
	{
		return;
	}

	UEdGraph* Graph = GetFocusedEdGraph();
	if (Graph == nullptr)
	{
		return;
	}

	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();

	PendingSize.RemoveAll(FBAUtils::IsNodeDeleted);

	// Save the currently viewport to restore once we are done
	if (PendingSize.Num() > 0 && !bFullyZoomed)
	{
		GraphEditor->GetViewLocation(ViewCache, ZoomCache);
		bFullyZoomed = true;

		if (GetDefault<UBASettings>()->bEnableCachingNodeSizeNotification && PendingSize.Num() > GetDefault<UBASettings>()->RequiredNumPendingSizeForNotification)
		{
			ShowCachingNotification();
		}
	}

	if (PendingSize.Num() > 0)
	{
		UEdGraphNode* FirstNode = PendingSize[0];

		if (FocusedNode != FirstNode)
		{
			DelayedCacheSizeTimeout.StartDelay(16);
			DelayedViewportZoomIn.StartDelay(2);
			FocusedNode = FirstNode;

			// Zoom fully in, to cache the node size
			GraphEditor->SetViewLocation(FVector2D(FocusedNode->NodePosX, FocusedNode->NodePosY), 1.f);
		}
		else
		{
			GraphEditor->SetViewLocation(FVector2D(FocusedNode->NodePosX, FocusedNode->NodePosY), 1.f);

			DelayedCacheSizeTimeout.Tick();
			if (DelayedCacheSizeTimeout.IsComplete())
			{
				NodeSizeTimeout -= DeltaTime;

				if (NodeSizeTimeout <= 0)
				{
					if (SizeTimeoutNotification.IsValid())
					{
						SizeTimeoutNotification.Pin()->SetCompletionState(SNotificationItem::CS_Fail);
						SizeTimeoutNotification.Pin()->ExpireAndFadeout();
					}
				}
			}
		}
	}

	// delay for two ticks to make sure the size is accurate
	DelayedViewportZoomIn.Tick();
	if (DelayedViewportZoomIn.IsActive())
	{
		return;
	}

	// cache node sizes
	TArray<UEdGraphNode*> NodesCalculated;
	for (UEdGraphNode* Node : PendingSize)
	{
		const bool bIsCommentNode = FBAUtils::IsCommentNode(Node);

		if (Node != FocusedNode)
		{
			// only cache the focused node resulting in more accurate node caching
			if (GetMutableDefault<UBASettings>()->bSlowButAccurateSizeCaching)
			{
				continue;
			}

			// comment nodes should only cache size if they are the focused node
			if (bIsCommentNode && Node != FocusedNode)
			{
				continue;
			}
		}

		if (FBAUtils::IsNodeDeleted(Node))
		{
			NodesCalculated.Add(Node);
			continue;
		}

		TSharedPtr<SGraphNode> GraphNode = GetGraphNode(Node);

		if (!GraphNode.IsValid())
		{
			continue;
		}

		// to calculate the node size, the graph node must be visible on screen (unless it is a comment node)
		if (!FBAUtils::IsNodeVisible(GraphPanel, Node) && !bIsCommentNode)
		{
			continue;
		}

		FVector2D Size = GraphNode->GetDesiredSize();

		// for comment nodes we only want to cache the title bar height
		if (FBAUtils::IsCommentNode(Node))
		{
			Size.Y = GraphNode->GetDesiredSizeForMarquee().Y;
		}

		// the size can be zero when a node is initially created, do not use this value
		if (Size.SizeSquared() <= 0)
		{
			continue;
		}

		// set each node to the global resize comment bubble setting
		if (!FBAUtils::IsCommentNode(Node)) // we don't want to do this for comment nodes, the auto size comments plugin should handle this setting
		{
			Node->bCommentBubblePinned = GetMutableDefault<UBASettings>()->bSetAllCommentBubblePinned;
		}

		// cache pin offset
		TArray<TSharedRef<SWidget>> PinsAsWidgets;
		GraphNode->GetPins(PinsAsWidgets);
		bool bAllPinsCached = true;

		FBANodeData NodeData;

		for (const TSharedRef<SWidget>& Widget : PinsAsWidgets)
		{
			TSharedPtr<SGraphPin> GraphPin = StaticCastSharedRef<SGraphPin>(Widget);
			if (GraphPin.IsValid())
			{
				if (UEdGraphPin* Pin = GraphPin->GetPinObj())
				{
					NodeData.CachedPins.Add(Pin->PinId, GraphPin->GetNodeOffset().Y);
				}
			}
			else
			{
				UE_LOG(LogBlueprintAssist, Error,
					TEXT("BlueprintAssistGraphHandler::UpdateCachedNodeSize: GraphPin is invalid for node %s"),
					*FBAUtils::GetNodeName(Node));

				bAllPinsCached = false;
				break;
			}
		}

		if (bAllPinsCached)
		{
			if (!Node->IsAutomaticallyPlacedGhostNode())
			{
				SNodePanel::SNode::FNodeSlot* CommentSlot = GraphNode->GetSlot(ENodeZone::TopCenter);
				if (CommentSlot != nullptr)
				{
					TSharedPtr<SCommentBubble> CommentBubble = StaticCastSharedRef<SCommentBubble>(CommentSlot->GetWidget());

					if (CommentBubble.IsValid())
					{
						FVector2D CommentBubbleSize = CommentBubble->GetDesiredSize();
						CommentBubbleSizeCache.Add(Node, CommentBubbleSize);
					}
				}
			}

			NodeData.CachedNodeSize = Size;
			GetGraphCache().CachedNodes.Add(Node->NodeGuid, NodeData);

			NodesCalculated.Add(Node);

			// Complete the size timeout notification
			if (SizeTimeoutNotification.IsValid())
			{
				SizeTimeoutNotification.Pin()->SetText(FText::FromString("Successfully calculated size"));
				SizeTimeoutNotification.Pin()->ExpireAndFadeout();
				SizeTimeoutNotification.Pin()->SetCompletionState(SNotificationItem::CS_Success);
			}

			if (bIsCommentNode && GetMutableDefault<UBASettings>()->bTryToHandleCommentNodes)
			{
				UEdGraphNode_Comment* CommentNode = Cast<UEdGraphNode_Comment>(Node);

				FSlateRect CommentRect = FBAUtils::GetNodeBounds(CommentNode);

				FChildren* PanelChildren = GetGraphPanel()->GetAllChildren();
				const int32 NumChildren = PanelChildren->Num();

				// Iterate across all nodes in the graph
				for (int32 NodeIndex = 0; NodeIndex < NumChildren; ++NodeIndex)
				{
					const TSharedRef<SGraphNode> SomeNodeWidget = StaticCastSharedRef<SGraphNode>(PanelChildren->GetChildAt(NodeIndex));
					UObject* GraphObject = SomeNodeWidget->GetObjectBeingDisplayed();

					if (GraphObject == nullptr)
					{
						continue;
					}

					// skip if we already contain the graph obj
					if (CommentNode->GetNodesUnderComment().Contains(GraphObject))
					{
						continue;
					}

					// check if the node bounds is contained in ourself
					if (GraphObject != CommentNode)
					{
						const FVector2D SomeNodePosition = SomeNodeWidget->GetPosition();
						if (CommentRect.ContainsPoint(SomeNodePosition))
						{
							CommentNode->AddNodeUnderComment(GraphObject);
						}
					}
				}
			}
		}
	}

	// remove any nodes that we calculated the size for
	for (UEdGraphNode* Node : NodesCalculated)
	{
		PendingSize.RemoveSwap(Node);
	}

	if (PendingSize.Num() == 0 && bFullyZoomed)
	{
		GetGraphEditor()->SetViewLocation(ViewCache, ZoomCache);
		bFullyZoomed = false;
		FocusedNode = nullptr;

		if (CachingNotification.IsValid())
		{
			CachingNotification.Pin()->SetCompletionState(SNotificationItem::CS_Success);
			CachingNotification.Pin()->ExpireAndFadeout();
		}
	}
}

void FBAGraphHandler::UpdateNodesRequiringFormatting()
{
	if (PendingFormatting.Num() == 0 && FormatAllColumns.Num() == 0)
	{
		return;
	}

	TArray<UEdGraphNode*> DeletedNodes = PendingFormatting.Array().FilterByPredicate(FBAUtils::IsNodeDeleted);
	for (UEdGraphNode* Node : DeletedNodes)
	{
		PendingFormatting.Remove(Node);
	}

	if (PendingSize.Num() > 0)
	{
		return;
	}

	const auto& GraphCacheCopy = GetGraphCache();
	const auto HasCachedSize = [GraphCacheCopy](UEdGraphNode* Node)
	{
		return GraphCacheCopy.CachedNodes.Contains(Node->NodeGuid);
	};

	TArray<UEdGraphNode*> NodesWithoutSize = PendingFormatting.Array().FilterByPredicate([&HasCachedSize](UEdGraphNode* Node) { return !HasCachedSize(Node); });

	if (NodesWithoutSize.Num() > 0)
	{
		bool bPendingSize = false;
		for (UEdGraphNode* Pending : PendingFormatting)
		{
			TSet<UEdGraphNode*> NodeTree = FBAUtils::GetNodeTree(Pending);
			bPendingSize |= UpdateNodeSizesChanges(NodeTree.Array());
		}

		if (bPendingSize)
		{
			return;
		}
	}

	// format dirty nodes
	TArray<UEdGraphNode*> NodesToFormatCopy = PendingFormatting.Array().FilterByPredicate(HasCachedSize);

	int CountError = NodesToFormatCopy.Num();

	while (NodesToFormatCopy.Num() > 0)
	{
		CountError -= 1;
		if (CountError < 0)
		{
			FNotificationInfo Notification(FText::FromString("Failed to format all nodes"));
			Notification.ExpireDuration = 2.0f;
			FSlateNotificationManager::Get().AddNotification(Notification)->SetCompletionState(SNotificationItem::CS_Fail);

			NodesToFormatCopy.Empty();
			PendingFormatting.Empty();
			break;
		}

		UEdGraphNode* NodeToFormat = NodesToFormatCopy.Pop();
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Formatting %s"), *FBAUtils::GetNodeName(NodeToFormat));

		check(GetGraphCache().CachedNodes.Contains(NodeToFormat->NodeGuid))

		TSharedPtr<FFormatterInterface> Formatter = FormatNodes(NodeToFormat);
		PendingFormatting.Remove(NodeToFormat);
		NodesToFormatCopy.Remove(NodeToFormat);

		if (Formatter.IsValid())
		{
			for (UEdGraphNode* Node : Formatter->GetFormattedNodes())
			{
				PendingFormatting.Remove(Node);
				NodesToFormatCopy.Remove(Node);
			}
		}

		if (ReplaceNewNodeTransaction.IsValid())
		{
			ReplaceNewNodeTransaction.Reset();
		}
	}

	// handle format all nodes
	if (FormatAllColumns.Num() > 0)
	{
		if (GetDefault<UBASettings>()->FormatAllStyle == EBAFormatAllStyle::Smart)
		{
			SmartFormatAll();
		}
		else
		{
			// this also handles EBAFormatAllStyle::NodeType, should separate into another function
			SimpleFormatAll();
		}
	}

	FormatterParameters.Reset();
	PendingTransaction.Reset();
}

void FBAGraphHandler::SimpleFormatAll()
{
	TSet<UEdGraphNode*> FormattedNodes;
	FSlateRect FormattedBounds;

	int32 ColumnX = 0;

	for (int i = 0; i < FormatAllColumns.Num(); ++i)
	{
		bool bFirst = true;

		for (UEdGraphNode* Node : FormatAllColumns[i])
		{
			if (FormattedNodes.Contains(Node))
			{
				continue;
			}

			Node->Modify();

			TSharedPtr<FFormatterInterface> Formatter = FormatNodes(Node, true);
			UEdGraphNode* RootNode = Formatter->GetRootNode();

			if (!Formatter.IsValid())
			{
				continue;
			}

			// align the position of the formatted nodes to the column
			const int32 DeltaX = ColumnX - RootNode->NodePosX;

			// offset the first formatted node's Y position to zero
			const int32 DeltaY = bFirst ? 0 - RootNode->NodePosY : 0;

			for (auto FormattedNode : Formatter->GetFormattedNodes())
			{
				FormattedNode->NodePosX += DeltaX;
				FormattedNode->NodePosY += DeltaY;
			}

			FormattedNodes.Append(Formatter->GetFormattedNodes());

			const FSlateRect CurrentBounds = FBAUtils::GetCachedNodeArrayBounds(AsShared(), Formatter->GetFormattedNodes().Array());

			if (bFirst)
			{
				bFirst = false;
				FormattedBounds = CurrentBounds;
			}
			else
			{
				const float Delta = (FormattedBounds.Bottom + GetMutableDefault<UBASettings>()->FormatAllPadding.Y) - CurrentBounds.Top;
				for (UEdGraphNode* FormattedNode : Formatter->GetFormattedNodes())
				{
					FormattedNode->NodePosY += Delta;
				}

				FormattedBounds = FormattedBounds.Expand(FBAUtils::GetCachedNodeArrayBounds(AsShared(), Formatter->GetFormattedNodes().Array()));
			}
		}

		if (!bFirst) // if bFirst is false that also means we formatted at least 1 node
		{
			ColumnX = FormattedBounds.Right + GetMutableDefault<UBASettings>()->FormatAllPadding.X;
		}
	}

	FormatAllColumns.Empty();
	FormatAllTransaction.Reset();
}

void FBAGraphHandler::SmartFormatAll()
{
	TArray<TSharedPtr<FFormatterInterface>> AllFormatters;

	// format all the nodes
	TSet<UEdGraphNode*> PreviouslyFormattedNodes;

	for (UEdGraphNode* Node : FormatAllColumns[0])
	{
		if (PreviouslyFormattedNodes.Contains(Node))
		{
			continue;
		}

		Node->Modify();

		TSharedPtr<FFormatterInterface> Formatter = FormatNodes(Node, true);
		AllFormatters.Add(Formatter);

		PreviouslyFormattedNodes.Append(Formatter->GetFormattedNodes());
	}

	float ColumnX = 0;
	while (AllFormatters.Num() > 0)
	{
		TArray<TSharedPtr<FFormatterInterface>> AllFormattersCopy = AllFormatters;

		// sort formatted nodes by left most
		AllFormattersCopy.Sort([](TSharedPtr<FFormatterInterface> FormatterA, TSharedPtr<FFormatterInterface> FormatterB)
		{
			UEdGraphNode* RootA = FormatterA->GetRootNode();
			UEdGraphNode* RootB = FormatterB->GetRootNode();
			if (RootA->NodePosX != RootB->NodePosX)
			{
				return RootA->NodePosX < RootB->NodePosX;
			}

			return RootA->NodePosY < RootB->NodePosY;
		});

		// get the bounds of the left most node
		TSharedPtr<FFormatterInterface> LeftMostNodeTree = AllFormattersCopy[0];
		const FSlateRect LeftMostNodeBounds = FBAUtils::GetCachedNodeArrayBounds(AsShared(), LeftMostNodeTree->GetFormattedNodes().Array());
		float ColumnRight = ColumnX + LeftMostNodeBounds.GetSize().X;

		TArray<TSharedPtr<FFormatterInterface>> CurrentColumn;
		CurrentColumn.Add(LeftMostNodeTree);

		// create columns by checking for overlapping formatted node-trees
		for (TSharedPtr<FFormatterInterface> Formatter : AllFormattersCopy)
		{
			if (Formatter == LeftMostNodeTree)
			{
				continue;
			}

			TSet<UEdGraphNode*> FormatterNodes = Formatter->GetFormattedNodes();
			FSlateRect Bounds = FBAUtils::GetCachedNodeArrayBounds(AsShared(), FormatterNodes.Array());

			if (Bounds.Left < ColumnRight)
			{
				ColumnRight = FMath::Max(ColumnRight, ColumnX + Bounds.GetSize().X);
				CurrentColumn.Add(Formatter);
			}
		}

		FSlateRect FormattedBounds;

		// Sort the column by height
		CurrentColumn.Sort([](TSharedPtr<FFormatterInterface> FormatterA, TSharedPtr<FFormatterInterface> FormatterB)
		{
			UEdGraphNode* RootA = FormatterA->GetRootNode();
			UEdGraphNode* RootB = FormatterB->GetRootNode();
			if (RootA->NodePosY != RootB->NodePosY)
			{
				return RootA->NodePosY < RootB->NodePosY;
			}

			return RootA->NodePosX < RootB->NodePosX;
		});

		bool bFirst = true;

		// position the node-trees into columns
		for (TSharedPtr<FFormatterInterface> Formatter : CurrentColumn)
		{
			UEdGraphNode* RootNode = Formatter->GetRootNode();

			// align the position of the formatted nodes to the column
			const int32 DeltaX = ColumnX - RootNode->NodePosX;

			// offset the first formatted node's Y position to zero
			const int32 DeltaY = bFirst ? 0 - RootNode->NodePosY : 0;

			for (auto FormattedNode : Formatter->GetFormattedNodes())
			{
				FormattedNode->NodePosX += DeltaX;
				FormattedNode->NodePosY += DeltaY;
			}

			const FSlateRect CurrentBounds = FBAUtils::GetCachedNodeArrayBounds(AsShared(), Formatter->GetFormattedNodes().Array());

			if (bFirst)
			{
				bFirst = false;
				FormattedBounds = CurrentBounds;
			}
			else
			{
				const float Delta = (FormattedBounds.Bottom + GetDefault<UBASettings>()->FormatAllPadding.Y) - CurrentBounds.Top;
				for (UEdGraphNode* FormattedNode : Formatter->GetFormattedNodes())
				{
					FormattedNode->NodePosY += Delta;
				}

				FormattedBounds = FormattedBounds.Expand(FBAUtils::GetCachedNodeArrayBounds(AsShared(), Formatter->GetFormattedNodes().Array()));
			}

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *FBAUtils::GetNodeName(Formatter->GetRootNode()));
			AllFormatters.Remove(Formatter);
		}

		ColumnX = ColumnRight + GetDefault<UBASettings>()->FormatAllPadding.X;
	}

	FormatAllColumns.Empty();
	FormatAllTransaction.Reset();
}

void FBAGraphHandler::SetSelectedPin(UEdGraphPin* NewPin)
{
	// if we changed pin, reset the color of the old selected pin
	if (SelectedPinHandle.IsSet() && !(SelectedPinHandle.GetValue() == FGraphPinHandle(NewPin)))
	{
		TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
		if (GraphPanel.IsValid())
		{
			UEdGraphPin* EdGraphPin = SelectedPinHandle->GetPinObj(*GraphPanel.Get());
			if (EdGraphPin != nullptr && !FBAUtils::IsNodeDeleted(EdGraphPin->GetOwningNodeUnchecked()))
			{
				TSharedPtr<SGraphPin> GraphPin = GetGraphPin(EdGraphPin);
				if (GraphPin.IsValid())
				{
					GraphPin->SetPinColorModifier(FLinearColor::White);
					GraphPin->SetColorAndOpacity(FLinearColor::White);
				}
			}
		}
	}

	SelectedPinHandle = FGraphPinHandle(NewPin);
}

void FBAGraphHandler::UpdateLerpViewport(const float DeltaTime)
{
	if (bLerpViewport)
	{
		FVector2D CurrentView;
		float CurrentZoom;
		GetGraphEditor()->GetViewLocation(CurrentView, CurrentZoom);

		TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
		if (!GraphPanel.IsValid())
		{
			return;
		}

		FVector2D TargetView = TargetLerpLocation;
		if (bCenterWhileLerping)
		{
			const FGeometry Geometry = GraphPanel->GetTickSpaceGeometry();
			const FVector2D HalfOfScreenInGraphSpace = 0.5f * Geometry.Size / GraphPanel->GetZoomAmount();
			TargetView -= HalfOfScreenInGraphSpace;
		}

		if (FVector2D::Distance(CurrentView, TargetView) > 10.f)
		{
			const FVector2D NewView = FMath::Vector2DInterpTo(CurrentView, TargetView, DeltaTime, 8.f);

			GetGraphEditor()->SetViewLocation(NewView, CurrentZoom);
		}
		else
		{
			bLerpViewport = false;
		}
	}
}

void FBAGraphHandler::BeginLerpViewport(const FVector2D TargetView, const bool bCenter)
{
	TargetLerpLocation = TargetView;
	bLerpViewport = true;
	bCenterWhileLerping = bCenter;
}

TSharedPtr<SGraphNode> FBAGraphHandler::GetGraphNode(UEdGraphNode* Node)
{
	TSharedPtr<SGraphPanel> GraphPanel = GetGraphPanel();
	if (GraphPanel.IsValid())
	{
		return GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
	}

	return nullptr;
}

TSharedPtr<SGraphPin> FBAGraphHandler::GetGraphPin(UEdGraphPin* Pin)
{
	if (TSharedPtr<SGraphNode> GraphNode = GetGraphNode(Pin->GetOwningNode()))
	{
		return GraphNode->FindWidgetForPin(Pin);
	}

	return nullptr;
}

void FBAGraphHandler::RefreshNodeSize(UEdGraphNode* Node)
{
	if (FBAUtils::IsKnotNode(Node))
	{
		return;
	}

	if (FBAUtils::IsGraphNode(Node))
	{
		GetGraphCache().CachedNodes.Remove(Node->NodeGuid);
		PendingSize.Add(Node);

		UEdGraphNode* NodeToFormat = GetRootNode(Node, TArray<UEdGraphNode*>());

		if (FormatterMap.Contains(NodeToFormat))
		{
			FormatterMap[NodeToFormat].Reset();
			FormatterMap.Remove(NodeToFormat);
		}
	}
	else if (FBAUtils::IsCommentNode(Node))
	{
		PendingSize.Add(Node);
	}
}

void FBAGraphHandler::RefreshAllNodeSizes()
{
	for (UEdGraphNode* Node : GetFocusedEdGraph()->Nodes)
	{
		RefreshNodeSize(Node);
	}
}

void FBAGraphHandler::ResetTransactions()
{
	ReplaceNewNodeTransaction.Reset();
	PendingTransaction.Reset();
	FormatAllTransaction.Reset();
}

void FBAGraphHandler::FormatAllEvents()
{
	UEdGraph* EdGraph = GetFocusedEdGraph();
	if (EdGraph == nullptr)
	{
		return;
	}

	const EBAFormatAllStyle FormatAllStyle = GetDefault<UBASettings>()->FormatAllStyle;

	TArray<UEdGraphNode*> ExtraNodes;
	TArray<UEdGraphNode*> CustomEvents;
	TArray<UEdGraphNode*> InputEvents;
	TArray<UEdGraphNode*> ActorEvents;
	TArray<UEdGraphNode*> ComponentEvents;
	TArray<UEdGraphNode*> OtherEvents;

	for (UEdGraphNode* Node : EdGraph->Nodes)
	{
		if (GetMutableDefault<UBASettings>()->FormatAllStyle == EBAFormatAllStyle::NodeType)
		{
			if (FBAUtils::IsExtraRootNode(Node))
			{
				ExtraNodes.Add(Node);
			}
			else if (Node->IsA(UK2Node_CustomEvent::StaticClass()))
			{
				CustomEvents.Add(Node);
			}
			else if (FBAUtils::IsInputNode(Node))
			{
				InputEvents.Add(Node);
			}
			else if (Node->IsA(UK2Node_ComponentBoundEvent::StaticClass()))
			{
				ComponentEvents.Add(Node);
			}
			else if (Node->IsA(UK2Node_Event::StaticClass())) // Node->IsA(UK2Node_ActorBoundEvent::StaticClass()) ||  
			{
				ActorEvents.Add(Node);
			}
			else if (FBAUtils::IsEventNode(Node))
			{
				OtherEvents.Add(Node);
			}
		}
		else
		{
			if (FBAUtils::IsEventNode(Node) || FBAUtils::IsExtraRootNode(Node))
			{
				OtherEvents.Add(Node);
			}
		}
	}

	if (FormatAllStyle == EBAFormatAllStyle::NodeType)
	{
		// TODO: Add setting to allow for user-defined columns
		FormatAllColumns = {
			ExtraNodes,
			ActorEvents,
			CustomEvents,
			InputEvents,
			ComponentEvents,
			OtherEvents
		};
	}
	else
	{
		FormatAllColumns = { OtherEvents };
	}

	const auto ExtraRootNodeSorter = [](UEdGraphNode& NodeA, UEdGraphNode& NodeB)
	{
		return FBAUtils::GetPinsByDirection(&NodeA, EGPD_Input).Num() < FBAUtils::GetPinsByDirection(&NodeB, EGPD_Input).Num();
	};

	const auto TopMostSorter = [](UEdGraphNode& NodeA, UEdGraphNode& NodeB)
	{
		return NodeA.NodePosY < NodeB.NodePosY;
	};

	bool bHasNodeToFormat = false;

	for (int i = 0; i < FormatAllColumns.Num(); ++i)
	{
		TArray<UEdGraphNode*>& Column = FormatAllColumns[i];

		for (UEdGraphNode* Node : Column)
		{
			if (GetDefault<UBASettings>()->bRefreshNodeSizeBeforeFormatting)
			{
				TSet<UEdGraphNode*> NodeTree = FBAUtils::GetNodeTree(Node);
				UpdateNodeSizesChanges(NodeTree.Array());
			}
		}

		if (!bHasNodeToFormat && Column.Num() > 0)
		{
			bHasNodeToFormat = true;
		}

		// TODO: Handle extra root nodes properly
		if (i == 0 && FormatAllStyle == EBAFormatAllStyle::NodeType)
		{
			ExtraNodes.StableSort(ExtraRootNodeSorter);
		}
		else
		{
			Column.Sort(TopMostSorter);
		}
	}

	if (bHasNodeToFormat)
	{
		FormatAllTransaction = MakeShareable(new FScopedTransaction(NSLOCTEXT("UnrealEd", "FormatAllNodes", "Format All Nodes")));
	}
}

void FBAGraphHandler::ApplyCommentBubbleSetting()
{
	if (UEdGraph* EdGraph = GetFocusedEdGraph())
	{
		for (UEdGraphNode* Node : EdGraph->Nodes)
		{
			Node->bCommentBubblePinned = GetMutableDefault<UBASettings>()->bSetAllCommentBubblePinned;
		}
	}
}

int32 FBAGraphHandler::GetNumberOfPendingNodesToCache() const
{
	return PendingSize.Num();
}

void FBAGraphHandler::ClearCache()
{
	PendingSize.Reset();
	PendingFormatting.Reset();
	DelayedViewportZoomIn.Cancel();
	DelayedCacheSizeTimeout.Cancel();
	FocusedNode = nullptr;
	bFullyZoomed = false;
	CachedGraphEditor.Pin()->SetViewLocation(ViewCache, ZoomCache);
}

void FBAGraphHandler::ClearFormatters()
{
	FormatterMap.Empty();
}

TSharedPtr<FFormatterInterface> FBAGraphHandler::FormatNodes(UEdGraphNode* Node, bool bUsingFormatAll)
{
	if (!GetGraphPanel().IsValid())
	{
		return nullptr;
	}

	if (!FBAUtils::IsGraphNode(Node))
	{
		return nullptr;
	}

	UEdGraph* EdGraph = GetFocusedEdGraph();
	if (EdGraph == nullptr)
	{
		return nullptr;
	}

	if (FBlueprintEditorUtils::IsGraphReadOnly(EdGraph))
	{
		return nullptr;
	}

	TSharedPtr<FFormatterInterface> Formatter;

	const bool bCheckSelectedNode = !bUsingFormatAll; // don't check selected node if we are running format all command
	UEdGraphNode* NodeToFormat = GetRootNode(Node, FormatterParameters.NodesToFormat, bCheckSelectedNode);

	// UE_LOG(LogTemp, Warning, TEXT("Using root node %s"), *FBAUtils::GetNodeName(NodeToFormat));

	const FName GraphClassName = EdGraph->GetClass()->GetFName();
	if (GetMutableDefault<UBASettings>()->UseBlueprintFormattingForTheseGraphs.Contains(GraphClassName))
	{
		if (FormatterMap.Contains(NodeToFormat) && GetMutableDefault<UBASettings>()->bEnableFasterFormatting)
		{
			Formatter = FormatterMap[NodeToFormat];
		}
		else
		{
			Formatter = MakeShared<FEdGraphFormatter>(AsShared(), FormatterParameters);
			FormatterMap.Add(NodeToFormat, Formatter);
		}
	}
	else
	{
		Formatter = MakeFormatter();
	}

	if (Formatter.IsValid())
	{
		// const double StartTime = FPlatformTime::Seconds();
		Formatter->FormatNode(NodeToFormat);
		// const double EndTime = FPlatformTime::Seconds();

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Formatting Took %f seconds for node %s (%d nodes) on graph %s"),
		//        EndTime - StartTime,
		//        *FBAUtils::GetNodeName(Node),
		//        Formatter->GetFormattedNodes().Num(),
		//        *GraphClassName.ToString());
	}

	return Formatter;
}

void FBAGraphHandler::CancelProcessingNodeSizes()
{
	PendingSize.Reset();
	PendingFormatting.Reset();

	if (bFullyZoomed)
	{
		GetGraphEditor()->SetViewLocation(ViewCache, ZoomCache);
		bFullyZoomed = false;
		FocusedNode = nullptr;
	}

	ResetTransactions();
}
