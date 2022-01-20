// Copyright 2021 fpwong. All Rights Reserved.

#include "EdGraphFormatter.h"

#include "BlueprintAssistGlobals.h"
#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistUtils.h"
#include "CommentSubGraphFormatter.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphParameterFormatter.h"
#include "GraphFormatterTypes.h"
#include "SGraphNodeComment.h"
#include "SGraphPanel.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor/BlueprintGraph/Classes/K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"

FNodeChangeInfo::FNodeChangeInfo(UEdGraphNode* InNode, UEdGraphNode* InNodeToKeepStill)
	: Node(InNode)
{
	bIsNodeToKeepStill = Node == InNodeToKeepStill;
	UpdateValues(InNodeToKeepStill);
}

void FNodeChangeInfo::UpdateValues(UEdGraphNode* NodeToKeepStill)
{
	NodeX = Node->NodePosX;
	NodeY = Node->NodePosY;

	NodeOffsetX = Node->NodePosX - NodeToKeepStill->NodePosX;
	NodeOffsetY = Node->NodePosY - NodeToKeepStill->NodePosY;

	Links.Empty();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			Links.Add(FPinLink(Pin, LinkedPin));
		}
	}
}

bool FNodeChangeInfo::HasChanged(UEdGraphNode* NodeToKeepStill)
{
	// check pin links
	TSet<FPinLink> NewLinks;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			NewLinks.Add(FPinLink(Pin, LinkedPin));
		}
	}

	if (NewLinks.Num() != Links.Num())
	{
		//UE_LOG(LogBlueprintAssist, Warning, TEXT("Num links changed"));
		return true;
	}

	for (const FPinLink& Link : Links)
	{
		if (!NewLinks.Contains(Link))
		{
			//UE_LOG(LogBlueprintAssist, Warning, TEXT("New links does not contain %s"), *Link.ToString());
			return true;
		}
	}

	return false;
}

FString ChildBranch::ToString() const
{
	return FString::Printf(TEXT("%s | %s"), *FBAUtils::GetPinName(Pin), *FBAUtils::GetPinName(ParentPin));
}

FEdGraphFormatter::FEdGraphFormatter(
	TSharedPtr<FBAGraphHandler> InGraphHandler,
	FEdGraphFormatterParameters InFormatterParameters)
	: GraphHandler(InGraphHandler)
	, RootNode(nullptr)
	, FormatterParameters(InFormatterParameters)
{
	const UBASettings* BASettings = GetDefault<UBASettings>();

	FormatterDirection = EGPD_Output;

	NodePadding = BASettings->BlueprintFormatterSettings.Padding;
	PinPadding = BASettings->BlueprintParameterPadding;
	TrackSpacing = BASettings->BlueprintKnotTrackSpacing;
	VerticalPinSpacing = BASettings->VerticalPinSpacing;
	bCenterBranches = BASettings->bCenterBranches;
	NumRequiredBranches = BASettings->NumRequiredBranches;

	LastFormattedX = 0;
	LastFormattedY = 0;
}

void FEdGraphFormatter::FormatNode(UEdGraphNode* InitialNode)
{
	if (!IsInitialNodeValid(InitialNode))
	{
		return;
	}

	RootNode = InitialNode;

	TArray<UEdGraphNode*> NewNodeTree = GetNodeTree(InitialNode);

	NodeTree = NewNodeTree;

	const auto& SelectedNodes = GraphHandler->GetSelectedNodes();
	const bool bAreAllNodesSelected = !NewNodeTree.ContainsByPredicate([&SelectedNodes](UEdGraphNode* Node)
	{
		return !SelectedNodes.Contains(Node);
	});

	GraphHandler->GetFocusedEdGraph()->Modify();

	// check if we can do simple relative formatting
	if (GetMutableDefault<UBASettings>()->bEnableFasterFormatting && !IsFormattingRequired(NewNodeTree))
	{
		SimpleRelativeFormatting();
		return;
	}

	NodeChangeInfos.Reset();
	NodePool.Reset();
	MainParameterFormatter.Reset();
	ParameterFormatterMap.Reset();
	KnotNodesSet.Reset();
	KnotTracks.Reset();
	FormatXInfoMap.Reset();
	Path.Reset();
	SameRowMapping.Reset();
	KnotNodeOwners.Reset();
	ParentComments.Reset();
	SubGraphFormatters.Reset();
	NodesToExpand.Reset();
	ParameterParentMap.Reset();
	NodeHeightLevels.Reset();

	if (FBAUtils::GetLinkedPins(RootNode).Num() == 0)
	{
		NodePool = { RootNode };
		return;
	}

	RemoveKnotNodes();

	NodeToKeepStill = FormatterParameters.NodeToKeepStill ? FormatterParameters.NodeToKeepStill : RootNode;
	// UE_LOG(LogTemp, Warning, TEXT("Node to keep still %s | Root %s"), *FBAUtils::GetNodeName(NodeToKeepStill), *FBAUtils::GetNodeName(RootNode));

	if (FBAUtils::IsNodePure(RootNode))
	{
		MainParameterFormatter = MakeShared<FEdGraphParameterFormatter>(GraphHandler, RootNode, SharedThis(this), NodeToKeepStill);
		MainParameterFormatter->FormatNode(RootNode);
		return;
	}

	const FVector2D SavedLocation = FVector2D(NodeToKeepStill->NodePosX, NodeToKeepStill->NodePosY);

	// initialize the node pool from the root node
	InitNodePool();

	InitCommentNodeInfo();

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Selected Root Node as %s | NodeToKeepStill as %s"), *FBAUtils::GetNodeName(RootNode), *FBAUtils::GetNodeName(NodeToKeepStill));
	//for (UEdGraphNode* Node : FormatterParameters.NodesToFormat)
	//{
	//	UE_LOG(LogBlueprintAssist, Warning, TEXT("\tSelective Formatting %s"), *FBAUtils::GetNodeName(Node));
	//}

	// for (UEdGraphNode* Node : NodePool)
	// {
	// 	// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tNodePool %s"), *FBAUtils::GetNodeName(Node));
	// }

	GetPinsOfSameHeight();

	bAccountForComments = false;
	FormatX(false);

	//UE_LOG(LogBlueprintAssist, Warning, TEXT("Path: "));
	//for (FPinLink& PinLink : Path)
	//{
	//	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *PinLink.ToString());
	//}

	//UE_LOG(LogBlueprintAssist, Warning, TEXT("NodeInfos: "));
	//for (UEdGraphNode* Node : NodePool)
	//{
	//	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *FBAUtils::GetNodeName(Node));
	//	if (!FormatXInfoMap.Contains(Node))
	//	{
	//		UE_LOG(LogBlueprintAssist, Error, TEXT("ERROR FormatXInfo does not contain %s"), *FBAUtils::GetNodeName(Node));
	//	}
	//	else
	//	{
	//		for (TSharedPtr<FFormatXInfo> Info : FormatXInfoMap[Node]->Children)
	//		{
	//			UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t%s"), *FBAUtils::GetNodeName(Info->GetNode()));
	//		}
	//	}
	//}

	if (GetMutableDefault<UBASettings>()->bCustomDebug == 0)
	{
		return;
	}

	/** Format the input nodes before we format the X position so we can get the column bounds */
	bAccountForComments = GetDefault<UBASettings>()->bAccountForCommentsWhenFormatting;
	FormatParameterNodes();

	if (bAccountForComments)
	{
		FormatComments();
	}

	if (GetMutableDefault<UBASettings>()->bCustomDebug == 3)
	{
		return;
	}

	Path.Empty();
	FormatXInfoMap.Empty();
	FormatX(true);

	if (GetDefault<UBASettings>()->bExpandNodesAheadOfParameters)
	{
		ExpandNodesAheadOfParameters();
	}

	if (GetMutableDefault<UBASettings>()->bCustomDebug == 1)
	{
		return;
	}

	/** Format Y (Rows) */
	FormatY();

	if (GetMutableDefault<UBASettings>()->bCustomDebug == 2)
	{
		return;
	}

	if (GetMutableDefault<UBASettings>()->bExpandNodesByHeight)
	{
		ExpandByHeight();
	}

	// TODO: Finish logic for wrapping nodes
	// WrapNodes();

	/** Format knot nodes */
	if (GetMutableDefault<UBASettings>()->bCreateKnotNodes)
	{
		FormatKnotNodes();
	}

	// Cleanup knot node pool
	for (auto KnotNode : KnotNodePool)
	{
		if (FBAUtils::GetLinkedNodes(KnotNode).Num() == 0)
		{
			FBAUtils::DeleteNode(KnotNode);
		}
	}
	KnotNodePool.Empty();

	/** Formatting may move nodes, move all nodes back using the root as a baseline */
	ResetRelativeToNodeToKeepStill(SavedLocation);

	if (GetDefault<UBASettings>()->bSnapToGrid)
	{
		/** Snap all nodes to the grid (only on the x-axis) */
		TSet<UEdGraphNode*> FormattedNodes = GetFormattedGraphNodes();
		for (UEdGraphNode* Node : FormattedNodes)
		{
			Node->NodePosX = FBAUtils::SnapToGrid(Node->NodePosX);
		}
	}

	SaveFormattingEndInfo();

	ModifyCommentNodes();

	// Check if formatting is required checks the difference between the node trees, so we must set it here
	NodeTree = GetNodeTree(InitialNode);

	//for (UEdGraphNode* Nodes : GetFormattedGraphNodes())
	//{
	//	UE_LOG(LogBlueprintAssist, Warning, TEXT("Formatted node %s"), *FBAUtils::GetNodeName(Nodes));
	//}
	//

	if (bAreAllNodesSelected)
	{
		auto& SelectionManager = GraphHandler->GetGraphPanel()->SelectionManager;
		for (auto Node : KnotNodesSet)
		{
			SelectionManager.SetNodeSelection(Node, true);
		}
	}
}

void FEdGraphFormatter::InitNodePool()
{
	NodePool.Empty();
	TArray<UEdGraphNode*> InputNodeStack;
	TArray<UEdGraphNode*> OutputNodeStack;
	OutputNodeStack.Push(RootNode);
	RootNode->Modify();

	while (InputNodeStack.Num() > 0 || OutputNodeStack.Num() > 0)
	{
		UEdGraphNode* CurrentNode
			= OutputNodeStack.Num() > 0
			? OutputNodeStack.Pop()
			: InputNodeStack.Pop();

		if (!GraphHandler->FilterSelectiveFormatting(CurrentNode, FormatterParameters.NodesToFormat))
		{
			continue;
		}

		if (NodePool.Contains(CurrentNode) || FBAUtils::IsNodePure(CurrentNode))
		{
			continue;
		}

		NodePool.Add(CurrentNode);

		TArray<EEdGraphPinDirection> Directions = { EGPD_Input, EGPD_Output };

		for (EEdGraphPinDirection& Dir : Directions)
		{
			TArray<UEdGraphPin*> ExecPins = FBAUtils::GetLinkedPins(CurrentNode, Dir).FilterByPredicate(IsExecOrDelegatePin);

			for (int32 MyPinIndex = ExecPins.Num() - 1; MyPinIndex >= 0; MyPinIndex--)
			{
				UEdGraphPin* Pin = ExecPins[MyPinIndex];

				for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; i--)
				{
					UEdGraphPin* LinkedPin = Pin->LinkedTo[i];
					UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();

					if (NodePool.Contains(LinkedNode) ||
						FBAUtils::IsNodePure(LinkedNode) ||
						!GraphHandler->FilterSelectiveFormatting(LinkedNode, FormatterParameters.NodesToFormat))
					{
						continue;
					}

					LinkedNode->Modify();

					FBAUtils::StraightenPin(GraphHandler, Pin, LinkedPin);

					if (Dir == EGPD_Output)
					{
						OutputNodeStack.Push(LinkedNode);
					}
					else
					{
						InputNodeStack.Push(LinkedNode);
					}
				}
			}
		}
	}
}

void FEdGraphFormatter::ExpandPendingNodes(bool bUseParameter)
{
	for (TSharedPtr<FFormatXInfo> Info : NodesToExpand)
	{
		if (!Info->Parent.IsValid())
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Expand X Invalid %s"), *FBAUtils::GetNodeName(Info->GetNode()));
			return;
		}

		UEdGraphNode* Node = Info->GetNode();
		UEdGraphNode* Parent = Info->Parent->GetNode();
		TArray<UEdGraphNode*> InputChildren = Info->GetChildren(EGPD_Input);

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Expand X %s | %s"), *FBAUtils::GetNodeName(Info->GetNode()), *FBAUtils::GetNodeName(Parent));

		if (InputChildren.Num() > 0)
		{
			FSlateRect InputBounds = bUseParameter
				? GetClusterBoundsForNodes(InputChildren)
				: FBAUtils::GetCachedNodeArrayBounds(GraphHandler, InputChildren);

			FSlateRect ParentBounds = bUseParameter
				? GetClusterBounds(Parent)
				: FBAUtils::GetCachedNodeBounds(GraphHandler, Parent);

			if (bAccountForComments)
			{
				InputBounds = GetRelativeBoundsForNodes(InputChildren, Parent, bUseParameter);
				ParentBounds = GetRelativeNodeBounds(Parent, Node, bUseParameter);
			}

			if (ParentBounds.Right > InputBounds.Left)
			{
				const float Delta = ParentBounds.Right - InputBounds.Left + NodePadding.X;

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Expanding node %s by %f"), *FBAUtils::GetNodeName(Node), Delta);

				Node->NodePosX += Delta;

				if (bUseParameter)
				{
					RefreshParameters(Node);
				}

				TArray<UEdGraphNode*> AllChildren = Info->GetChildren();
				for (UEdGraphNode* Child : AllChildren)
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tChild %s"), *FBAUtils::GetNodeName(Child));

					Child->NodePosX += Delta;

					if (bUseParameter)
					{
						RefreshParameters(Child);
					}
				}
			}
		}
	}
}

void FEdGraphFormatter::SimpleRelativeFormatting()
{
	for (UEdGraphNode* Node : GetFormattedNodes())
	{
		// check(NodeChangeInfos.Contains(Node))
		if (NodeChangeInfos.Contains(Node))
		{
			Node->NodePosX = NodeToKeepStill->NodePosX + NodeChangeInfos[Node].NodeOffsetX;
			Node->NodePosY = NodeToKeepStill->NodePosY + NodeChangeInfos[Node].NodeOffsetY;
		}
		else
		{
			UE_LOG(LogBlueprintAssist, Error, TEXT("No ChangeInfo for %s"), *FBAUtils::GetNodeName(Node));
		}
	}

	SaveFormattingEndInfo();

	ModifyCommentNodes();
}

void FEdGraphFormatter::FormatX(const bool bUseParameter)
{
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("----- Format X -----"));

	TSet<UEdGraphNode*> VisitedNodes;
	TSet<UEdGraphNode*> PendingNodes;
	PendingNodes.Add(RootNode);
	TSet<FPinLink> VisitedLinks;
	const FPinLink RootNodeLink(nullptr, nullptr, RootNode);
	TSharedPtr<FFormatXInfo> RootInfo = MakeShared<FFormatXInfo>(RootNodeLink, nullptr);

	TArray<TSharedPtr<FFormatXInfo>> OutputStack;
	TArray<TSharedPtr<FFormatXInfo>> InputStack;
	OutputStack.Push(RootInfo);
	FormatXInfoMap.Add(RootNode, RootInfo);

	EEdGraphPinDirection LastDirection = EGPD_Output;

	NodesToExpand.Reset();

	while (OutputStack.Num() > 0 || InputStack.Num() > 0)
	{
		// try to get the current info from the pending input
		TSharedPtr<FFormatXInfo> CurrentInfo = nullptr;

		TArray<TSharedPtr<FFormatXInfo>>& FirstStack = LastDirection == EGPD_Output ? OutputStack : InputStack;
		TArray<TSharedPtr<FFormatXInfo>>& SecondStack = LastDirection == EGPD_Output ? InputStack : OutputStack;

		if (FirstStack.Num() > 0)
		{
			CurrentInfo = FirstStack.Pop();
		}
		else
		{
			CurrentInfo = SecondStack.Pop();
		}

		LastDirection = CurrentInfo->Link.GetDirection();

		UEdGraphNode* CurrentNode = CurrentInfo->GetNode();
		VisitedNodes.Add(CurrentNode);

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Processing %s | %s"), *FBAUtils::GetNodeName(CurrentNode), *CurrentInfo->Link.ToString());
		const int32 NewX = GetChildX(CurrentInfo->Link, bUseParameter);

		if (!FormatXInfoMap.Contains(CurrentNode))
		{
			if (CurrentNode != RootNode)
			{
				CurrentInfo->SetParent(CurrentInfo->Parent);
				CurrentNode->NodePosX = NewX;

				if (bUseParameter)
				{
					if (SubGraphFormatters.Contains(CurrentNode))
					{
						TSharedPtr<FCommentSubGraphFormatter> SubGraphFormatter = SubGraphFormatters[CurrentNode];
						if (!SubGraphFormatter->bHasBeenFormatted)
						{
							SubGraphFormatters[CurrentNode]->FormatNode(CurrentNode);
							// UE_LOG(LogBlueprintAssist, Warning, TEXT("Formatted sub graph for %s"), *FBAUtils::GetNodeName(CurrentNode));
						}
					}

					RefreshParameters(CurrentNode);
				}

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tInitial Set node pos x %d %s"), NewX, *FBAUtils::GetNodeName(CurrentNode));

				Path.Add(CurrentInfo->Link);
			}
			FormatXInfoMap.Add(CurrentNode, CurrentInfo);
		}
		else
		{
			TSharedPtr<FFormatXInfo> OldInfo = FormatXInfoMap[CurrentNode];

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tInfo map contains %s | %s (%s) | Parent %s (%s) | %d"),
			//        *FBAUtils::GetNodeName(CurrentInfo->Link.To->GetOwningNode()),
			//        *FBAUtils::GetNodeName(CurrentInfo->GetNode()),
			//        *FBAUtils::GetPinName(CurrentInfo->Link.To),
			//        *FBAUtils::GetNodeName(CurrentInfo->Link.From->GetOwningNode()),
			//        *FBAUtils::GetPinName(CurrentInfo->Link.From),
			//        NewX);

			const bool bHasNoParent = CurrentInfo->Link.From == nullptr;

			bool bHasCycle = false;
			if (!bHasNoParent) // if we have a parent, check if there is a cycle
			{
				// bHasCycle = OldInfo->GetChildren(EGPD_Output).Contains(CurrentInfo->Parent->GetNode());
				bHasCycle = OldInfo->GetChildren().Contains(CurrentInfo->Parent->GetNode());

				// if (bHasCycle)
				// {
				// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tHas cycle! Skipping"));
				// 	for (UEdGraphNode* Child : OldInfo->GetChildren(EGPD_Output))
				// 	{
				// 		UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tChild: %s"), *FBAUtils::GetNodeName(Child));
				// 	}
				// }

				// for (UEdGraphNode* Child : OldInfo->GetChildren(EGPD_Output))
				// // for (UEdGraphNode* Child : OldInfo->GetChildren())
				// {
				// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tParent %s | Child: %s"), *FBAUtils::GetNodeName(CurrentInfo->Parent->GetNode()), *FBAUtils::GetNodeName(Child));
				// }
			}

			if (bHasNoParent || !bHasCycle)
			{
				if (OldInfo->Parent.IsValid())
				{
					bool bTakeNewParent = bHasNoParent;

					if (!bTakeNewParent)
					{
						const int32 OldX = CurrentInfo->GetNode()->NodePosX;

						const bool bPositionIsBetter
							= CurrentInfo->Link.From->Direction == EGPD_Output
							? NewX > OldX
							: NewX < OldX;

						// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t Comparing parents Old: %s (%d) New: %s (%d)"),
						//        *FBAUtils::GetNodeName(OldInfo->Link.From->GetOwningNode()), OldX,
						//        *FBAUtils::GetNodeName(CurrentInfo->Link.From->GetOwningNode()), NewX);

						const bool bSameDirection = OldInfo->Link.To->Direction == CurrentInfo->Link.To->Direction;
						// if (!bSameDirection) UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tNot same direction"));
						//
						// if (!bPositionIsBetter) UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tPosition is worse?"));

						bTakeNewParent = bPositionIsBetter && bSameDirection;
					}

					// take the new parent by updating the old info
					if (bTakeNewParent)
					{
						// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tTOOK PARENT"));

						OldInfo->Link = CurrentInfo->Link;
						OldInfo->SetParent(CurrentInfo->Parent);

						CurrentInfo = OldInfo;

						CurrentNode->NodePosX = NewX;
						if (bUseParameter)
						{
							RefreshParameters(CurrentNode);
						}

						for (TSharedPtr<FFormatXInfo> ChildInfo : CurrentInfo->Children)
						{
							if (ChildInfo->Link.GetDirection() == EGPD_Output)
							{
								OutputStack.Push(ChildInfo);
							}
							else
							{
								InputStack.Push(ChildInfo);
							}
						}

						Path.Add(CurrentInfo->Link);
					}
				}
			}
		}

		TArray<UEdGraphPin*> LinkedPins = FBAUtils::GetLinkedPins(CurrentInfo->GetNode()).FilterByPredicate(IsExecOrDelegatePin);

		for (int i = LinkedPins.Num() - 1; i >= 0; --i)
		{
			UEdGraphPin* ParentPin = LinkedPins[i];

			for (UEdGraphPin* LinkedPin : ParentPin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();

				const FPinLink PinLink(ParentPin, LinkedPin, LinkedNode);

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tTrying to iterate link %s"), *PinLink.ToString());

				if (VisitedLinks.Contains(PinLink))
				{
					continue;
				}

				VisitedLinks.Add(PinLink);
				if (!NodePool.Contains(LinkedNode))
				{
					continue;
				}

				if (FBAUtils::IsNodePure(LinkedNode))
				{
					continue;
				}

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tIterating pin link %s"), *PinLink.ToString());

				TSharedPtr<FFormatXInfo> LinkedInfo = MakeShared<FFormatXInfo>(PinLink, CurrentInfo);

				if (ParentPin->Direction == EGPD_Output)
				{
					OutputStack.Push(LinkedInfo);
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t\t\tAdded to output stack"));
				}
				else
				{
					if (GetMutableDefault<UBASettings>()->FormattingStyle == EBANodeFormattingStyle::Expanded)
					{
						const bool bHasCycle = PendingNodes.Contains(LinkedNode) || FBAUtils::GetExecTree(LinkedNode, EGPD_Input).Contains(CurrentInfo->GetNode());
						//FBAUtils::GetExecTree(LinkedNode, EGPD_Input).Array().FilterByPredicate(OnlySelected).Contains(CurrentInfo->GetNode());
						// const bool bHasCycle = FBAUtils::GetExecTree(LinkedNode, EGPD_Input).Array().FilterByPredicate(OnlySelected).Contains(CurrentInfo->GetNode());
						if (!bHasCycle)
						{
							if (CurrentInfo->Link.GetDirection() == EGPD_Output)
							{
								if (!CurrentInfo->Parent.IsValid() || LinkedNode != CurrentInfo->Parent->GetNode())
								{
									NodesToExpand.Add(CurrentInfo);
									// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t\t\tExpanding node %s"), *FBAUtils::GetNodeName(LinkedNode));
								}
							}
						}
					}

					InputStack.Push(LinkedInfo);
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t\t\tAdded to input stack"));
				}

				PendingNodes.Add(LinkedNode);
			}
		}
	}

	if (GetMutableDefault<UBASettings>()->FormattingStyle == EBANodeFormattingStyle::Expanded)
	{
		ExpandPendingNodes(bUseParameter);
	}
}

void FEdGraphFormatter::ExpandByHeight()
{
	// expand nodes in the output direction for centered branches
	for (UEdGraphNode* Node : NodePool)
	{
		TSharedPtr<FFormatXInfo> Info = FormatXInfoMap[Node];

		const TArray<FPinLink> PinLinks = Info->GetChildrenAsLinks(EGPD_Output);

		if (bCenterBranches && PinLinks.Num() < NumRequiredBranches)
		{
			continue;
		}

		float LargestExpandX = 0;
		for (const FPinLink& Link : PinLinks)
		{
			const FVector2D ToPos = FBAUtils::GetPinPos(GraphHandler, Link.To);
			const FVector2D FromPos = FBAUtils::GetPinPos(GraphHandler, Link.From);

			const float PinDeltaY = FMath::Abs(ToPos.Y - FromPos.Y);
			const float PinDeltaX = FMath::Abs(ToPos.X - FromPos.X);

			// expand to move the node to form a 45 degree angle for the wire (delta x == delta y)
			const float ExpandX = PinDeltaY * 0.75f - PinDeltaX;

			LargestExpandX = FMath::Max(ExpandX, LargestExpandX);
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Delta X %f | DeltaY %f | ExpandX %f"), PinDeltaX, PinDeltaY, ExpandX);
		}

		if (LargestExpandX <= 0)
		{
			continue;
		}

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Expanding %s"), *FBAUtils::GetNodeName(Node));
		TArray<UEdGraphNode*> Children = Info->GetChildren(EGPD_Output);
		for (UEdGraphNode* Child : Children)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tChild %s"), *FBAUtils::GetNodeName(Child));
			Child->NodePosX += LargestExpandX;
			RefreshParameters(Child);
		}
	}
}

void FEdGraphFormatter::InitCommentNodeInfo()
{
	ParentComments.Empty();
	CommentNodesContains.Reset();

	TArray<UEdGraphNode_Comment*> CommentNodes = FBAUtils::GetCommentNodesFromGraph(GraphHandler->GetFocusedEdGraph());

	CommentNodes.Sort([](UEdGraphNode_Comment& NodeA, UEdGraphNode_Comment& NodeB)
	{
		return NodeA.CommentDepth > NodeB.CommentDepth;
	});

	TSet<UEdGraphNode*> FormattedNodes = GetFormattedGraphNodes();

	for (UEdGraphNode_Comment* Comment : CommentNodes)
	{
		TArray<UEdGraphNode*> NodesUnderComment = FBAUtils::GetNodesUnderComment(Comment);

		bool bShouldModify = false;
		for (UEdGraphNode* EdGraphNode : NodesUnderComment)
		{
			if (NodeTree.Contains(EdGraphNode))
			{
				CommentNodesContains.FindOrAdd(Comment).Add(EdGraphNode);

				ParentComments.FindOrAdd(EdGraphNode).Add(Comment);

				bShouldModify = true;
			}
		}

		if (bShouldModify)
		{
			Comment->Modify();
		}
	}
}

void FEdGraphFormatter::ExpandNodesAheadOfParameters()
{
	for (UEdGraphNode* Node : NodePool)
	{
		TSharedPtr<FFormatXInfo> Info = FormatXInfoMap[Node];
		const TArray<FPinLink> PinLinks = Info->GetChildrenAsLinks(EGPD_Output);

		int32 LargestExpandX = 0;
		TArray<UEdGraphNode*> ParameterNodes = FBAUtils::GetLinkedNodes(Node, EGPD_Input).FilterByPredicate(FBAUtils::IsNodePure);

		for (UEdGraphNode* Param : ParameterNodes)
		{
			if (ParameterParentMap.Contains(Param))
			{
				const auto& ParamFormatter = ParameterParentMap[Param];

				// we only want to move ahead of nodes which aren't our children
				const bool bIsChild = ParamFormatter->GetRootNode() == Node;
				if (!bIsChild && !ParamFormatter->IsUsingHelixing())
				{
					const FSlateRect ParamNodeBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, Param);
					const int32 Delta = FMath::RoundToInt(ParamNodeBounds.Right + PinPadding.X - Node->NodePosX);
					if (Delta > 0)
					{
						LargestExpandX = FMath::Max(Delta, LargestExpandX);
						// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tExpand %s | Param %s, %d"), *FBAUtils::GetNodeName(Node), *FBAUtils::GetNodeName(Param), Delta);
					}
				}
			}
		}

		if (LargestExpandX <= 0)
		{
			continue;
		}

		Node->NodePosX += LargestExpandX;

		TArray<UEdGraphNode*> Children = Info->GetChildren(EGPD_Output);
		for (UEdGraphNode* Child : Children)
		{
			Child->NodePosX += LargestExpandX;
			RefreshParameters(Child);
		}
	}
}

void FEdGraphFormatter::FormatY_Recursive(
	UEdGraphNode* CurrentNode,
	UEdGraphPin* CurrentPin,
	UEdGraphPin* ParentPin,
	TSet<UEdGraphNode*>& NodesToCollisionCheck,
	TSet<FPinLink>& VisitedLinks,
	const bool bSameRow,
	TSet<UEdGraphNode*>& Children)
{
	// const FString NodeNameA = CurrentNode == nullptr
	// 	? FString("nullptr")
	// 	: FBAUtils::GetNodeName(CurrentNode);
	// const FString PinNameA = CurrentPin == nullptr ? FString("nullptr") : FBAUtils::GetPinName(CurrentPin);
	// const FString NodeNameB = ParentPin == nullptr
	// 	? FString("nullptr")
	// 	: FBAUtils::GetNodeName(ParentPin->GetOwningNode());
	// const FString PinNameB = ParentPin == nullptr ? FString("nullptr") : FBAUtils::GetPinName(ParentPin);
	//
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("FormatY Next : %s | %s || %s | %s"),
	//        *NodeNameA, *PinNameA,
	//        *NodeNameB, *PinNameB);

	for (int CollisionLimit = 0; CollisionLimit < 30; CollisionLimit++)
	{
		bool bNoCollision = true;

		TArray<UEdGraphNode*> NodesCopy = NodesToCollisionCheck.Array();
		while (NodesCopy.Num() > 0)
		{
			UEdGraphNode* NodeToCollisionCheck = NodesCopy.Pop();

			if (NodeToCollisionCheck == CurrentNode)
			{
				continue;
			}

			if (ParentPin != nullptr && ParentPin->GetOwningNode() == NodeToCollisionCheck)
			{
				continue;
			}

			TSet<UEdGraphNode*> NodesToMove;

			FSlateRect MyBounds = bAccountForComments ? GetRelativeNodeBounds(CurrentNode, NodeToCollisionCheck, NodesToMove, true) : GetClusterBounds(CurrentNode);
			const FMargin CollisionPadding(0, 0, 0, NodePadding.Y);

			FSlateRect OtherBounds = bAccountForComments ? GetRelativeNodeBounds(NodeToCollisionCheck, CurrentNode, true) : GetClusterBounds(NodeToCollisionCheck);

			OtherBounds = OtherBounds.ExtendBy(CollisionPadding);

			if (FSlateRect::DoRectanglesIntersect(MyBounds, OtherBounds))
			{
				bNoCollision = false;
				const int32 Delta = OtherBounds.Bottom - MyBounds.Top;

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Collision between %d | %s and %s"),
				// 	Delta + 1,
				// 	*FBAUtils::GetNodeName(CurrentNode),
				// 	*FBAUtils::GetNodeName(NodeToCollisionCheck));
				//
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *MyBounds.ToString());
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *OtherBounds.ToString());

				if (NodesToMove.Num() > 0)
				{
					for (UEdGraphNode* Node : NodesToMove)
					{
						// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tMoved node relative %s %d"), *FBAUtils::GetNodeName(Node), Delta + 1);
						Node->NodePosY += Delta + 1;
						RefreshParameters(Node);
					}
				}
				else
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tMoved node single %s"), *FBAUtils::GetNodeName(CurrentNode));
					CurrentNode->NodePosY += Delta + 1;
					RefreshParameters(CurrentNode);
				}

				for (auto Node : NodesToMove)
				{
					NodesCopy.Remove(Node);
				}
			}
		}

		if (bNoCollision)
		{
			break;
		}
	}

	NodesToCollisionCheck.Emplace(CurrentNode);

	const EEdGraphPinDirection ParentDirection = ParentPin == nullptr ? EGPD_Output : ParentPin->Direction.GetValue();

	bool bFirstPin = true;

	UEdGraphPin* MainPin = CurrentPin;

	bool bCenteredParent = false;

	TArray<EEdGraphPinDirection> Directions = { ParentDirection, UEdGraphPin::GetComplementaryDirection(ParentDirection) };
	for (EEdGraphPinDirection CurrentDirection : Directions)
	{
		TArray<UEdGraphPin*> Pins = FBAUtils::GetLinkedPins(CurrentNode, CurrentDirection)
			.FilterByPredicate(IsExecOrDelegatePin)
			.FilterByPredicate(FBAUtils::IsPinLinked);

		UEdGraphPin* LastLinked = CurrentPin;
		UEdGraphPin* LastProcessed = nullptr;

		TArray<ChildBranch> ChildBranches;

		int DeltaY = 0;
		for (UEdGraphPin* MyPin : Pins)
		{
			TArray<UEdGraphPin*> LinkedPins = MyPin->LinkedTo;

			for (int i = 0; i < LinkedPins.Num(); ++i)
			{
				UEdGraphPin* OtherPin = LinkedPins[i];
				UEdGraphNode* OtherNode = OtherPin->GetOwningNode();
				FPinLink Link(MyPin, OtherPin);

				bool bIsSameLink = Path.Contains(Link);

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tIter Child %s"), *FBAUtils::GetNodeName(OtherNode));
				//
				// if (!bIsSameLink)
				// {
				// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tNot same link!"));
				// }

				if (VisitedLinks.Contains(Link)
					|| !NodePool.Contains(OtherNode)
					|| FBAUtils::IsNodePure(OtherNode)
					|| NodesToCollisionCheck.Contains(OtherNode)
					|| !bIsSameLink)
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tSkipping child"));
					continue;
				}
				VisitedLinks.Add(Link);

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tTaking Child %s"), *FBAUtils::GetNodeName(OtherNode));

				FBAUtils::StraightenPin(GraphHandler, MyPin, OtherPin);

				bool bChildIsSameRow = false;

				if (bFirstPin && (ParentPin == nullptr || MyPin->Direction == ParentPin->Direction))
				{
					bChildIsSameRow = true;
					bFirstPin = false;
					// UE_LOG(LogBlueprintAssist, Error, TEXT("\t\tNode %s is same row as %s"),
					//        *FBAUtils::GetNodeName(OtherNode),
					//        *FBAUtils::GetNodeName(CurrentNode));
				}
				else
				{
					if (LastProcessed != nullptr)
					{
						//UE_LOG(LogBlueprintAssist, Warning, TEXT("Moved node %s to %s"), *FBAUtils::GetNodeName(OtherNode), *FBAUtils::GetNodeName(LastPinOther->GetOwningNode()));
						const int32 NewNodePosY = FMath::Max(OtherNode->NodePosY, LastProcessed->GetOwningNode()->NodePosY);
						FBAUtils::SetNodePosY(GraphHandler, OtherNode, NewNodePosY);
					}
				}

				if (!NodeHeightLevels.Contains(OtherNode))
				{
					int NewHeight = NodeHeightLevels[CurrentNode] + (bChildIsSameRow ? 0 : DeltaY);

					NodeHeightLevels.Add(OtherNode, NewHeight);

					// UE_LOG(LogBlueprintAssist, Warning, TEXT("Set height for node %s to %d"), *FBAUtils::GetNodeName(OtherNode), NewHeight);
				}

				RefreshParameters(OtherNode);

				TSet<UEdGraphNode*> LocalChildren;
				FormatY_Recursive(OtherNode, OtherPin, MyPin, NodesToCollisionCheck, VisitedLinks, bChildIsSameRow, LocalChildren);
				Children.Append(LocalChildren);

				if (FormatXInfoMap[CurrentNode]->GetImmediateChildren().Contains(OtherNode))
				{
					ChildBranches.Add(ChildBranch(OtherPin, MyPin, LocalChildren));
				}

				//UE_LOG(LogBlueprintAssist, Warning, TEXT("Local children for %s"), *FBAUtils::GetNodeName(CurrentNode));
				//for (UEdGraphNode* Node : LocalChildren)
				//{
				//	UE_LOG(LogBlueprintAssist, Warning, TEXT("\tChild %s"), *FBAUtils::GetNodeName(Node));
				//}

				if (!bChildIsSameRow && LocalChildren.Num() > 0)
				{
					UEdGraphPin* PinToAvoid = LastLinked;
					if (MainPin != nullptr)
					{
						PinToAvoid = MainPin;
						MainPin = nullptr;
					}

					if (PinToAvoid != nullptr && GetDefault<UBASettings>()->bCustomDebug != 27)
					{
						TSet<UEdGraphNode*> NodesToMove;
						FSlateRect Bounds = bAccountForComments ? GetRelativeBoundsForNodes(LocalChildren.Array(), CurrentNode, NodesToMove, true) : FBAUtils::GetCachedNodeArrayBounds(GraphHandler, LocalChildren.Array());

						// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tPin to avoid %s (%s)"), *FBAUtils::GetPinName(PinToAvoid), *FBAUtils::GetPinName(OtherPin));
						const float PinPos = GraphHandler->GetPinY(PinToAvoid) + VerticalPinSpacing;
						const float Delta = PinPos - Bounds.Top;

						if (Delta > 0)
						{
							if (NodesToMove.Num() > 0)
							{
								for (auto Node : NodesToMove)
								{
									Node->NodePosY += Delta;
								}
							}
							else
							{
								for (UEdGraphNode* Child : LocalChildren)
								{
									Child->NodePosY += Delta;
									RefreshParameters(Child);
								}
							}
						}
					}
				}

				LastProcessed = OtherPin;
			}

			LastLinked = MyPin;

			DeltaY += 1;
		}

		if (bCenterBranches && ChildBranches.Num() >= NumRequiredBranches && ParentDirection == EGPD_Output)
		{
			if (CurrentDirection != ParentDirection)
			{
				bCenteredParent = true;
			}

			CenterBranches(CurrentNode, ChildBranches, NodesToCollisionCheck);
		}
	}

	Children.Add(CurrentNode);

	if (bSameRow && ParentPin != nullptr && !bCenteredParent)
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tStraightening pin from %s to %s"),
		//        *FBAUtils::GetPinName(CurrentPin),
		//        *FBAUtils::GetPinName(ParentPin));

		FBAUtils::StraightenPin(GraphHandler, CurrentPin, ParentPin);
		RefreshParameters(ParentPin->GetOwningNode());
	}
}

void FEdGraphFormatter::GetPinsOfSameHeight_Recursive(
	UEdGraphNode* CurrentNode,
	UEdGraphPin* CurrentPin,
	UEdGraphPin* ParentPin,
	TSet<UEdGraphNode*>& NodesToCollisionCheck,
	TSet<FPinLink>& VisitedLinks)
{
	NodesToCollisionCheck.Emplace(CurrentNode);

	TArray<TArray<UEdGraphPin*>> OutputInput;

	const EEdGraphPinDirection Direction = CurrentPin == nullptr ? EGPD_Output : CurrentPin->Direction.GetValue();

	OutputInput.Add(FBAUtils::GetLinkedPins(CurrentNode, Direction).FilterByPredicate(IsExecOrDelegatePin));
	OutputInput.Add(FBAUtils::GetLinkedPins(CurrentNode, UEdGraphPin::GetComplementaryDirection(Direction)).FilterByPredicate(IsExecOrDelegatePin));

	bool bFirstPin = true;

	auto& GraphHandlerCapture = GraphHandler;

	auto LinkedToSorter = [&GraphHandlerCapture, &NodesToCollisionCheck](UEdGraphPin& PinA, UEdGraphPin& PinB)
	{
		struct FLocal
		{
			static void GetPins(UEdGraphPin* NextPin, TSet<UEdGraphNode*>& VisitedNodes, TArray<UEdGraphPin*>& OutPins, bool& bHasEventNode, int32& DepthToEventNode, int32 TempDepth)
			{
				if (FBAUtils::IsEventNode(NextPin->GetOwningNode()))
				{
					DepthToEventNode = TempDepth;
					bHasEventNode = true;
				}

				if (VisitedNodes.Contains(NextPin->GetOwningNode()))
				{
					OutPins.Add(NextPin);
					return;
				}

				VisitedNodes.Add(NextPin->GetOwningNode());

				auto NextPins = FBAUtils::GetLinkedToPins(NextPin->GetOwningNode(), EGPD_Input);

				for (UEdGraphPin* Pin : NextPins)
				{
					GetPins(Pin, VisitedNodes, OutPins, bHasEventNode, DepthToEventNode, TempDepth + 1);
				}
			}

			static UEdGraphPin* HighestPin(TSharedPtr<FBAGraphHandler> GraphHandler, UEdGraphPin* Pin, TSet<UEdGraphNode*>& VisitedNodes, bool& bHasEventNode, int32& DepthToEventNode)
			{
				TArray<UEdGraphPin*> OutPins;
				GetPins(Pin, VisitedNodes, OutPins, bHasEventNode, DepthToEventNode, 0);

				if (OutPins.Num() == 0)
				{
					return nullptr;
				}

				OutPins.StableSort([GraphHandler](UEdGraphPin& PinA, UEdGraphPin& PinB)
				{
					const FVector2D PinPosA = FBAUtils::GetPinPos(GraphHandler, &PinA);
					const FVector2D PinPosB = FBAUtils::GetPinPos(GraphHandler, &PinB);

					if (PinPosA.X != PinPosB.X)
					{
						return PinPosA.X < PinPosB.X;
					}

					return PinPosA.Y < PinPosB.Y;
				});

				return OutPins[0];
			}
		};

		bool bHasEventNodeA = false;
		int32 DepthToEventNodeA = 0;

		auto VisitedNodesCopyA = NodesToCollisionCheck;
		UEdGraphPin* HighestPinA = FLocal::HighestPin(GraphHandlerCapture, &PinA, VisitedNodesCopyA, bHasEventNodeA, DepthToEventNodeA);
		bool bHasEventNodeB = false;
		int32 DepthToEventNodeB = 0;
		auto VisitedNodesCopyB = NodesToCollisionCheck;
		UEdGraphPin* HighestPinB = FLocal::HighestPin(GraphHandlerCapture, &PinB, VisitedNodesCopyB, bHasEventNodeB, DepthToEventNodeB);

		if (HighestPinA == nullptr || HighestPinB == nullptr)
		{
			if (bHasEventNodeA != bHasEventNodeB)
			{
				return bHasEventNodeA > bHasEventNodeB;
			}

			return DepthToEventNodeA > DepthToEventNodeB;
		}

		const FVector2D PinPosA = FBAUtils::GetPinPos(GraphHandlerCapture, HighestPinA);
		const FVector2D PinPosB = FBAUtils::GetPinPos(GraphHandlerCapture, HighestPinB);

		if (PinPosA.X != PinPosB.X)
		{
			return PinPosA.X < PinPosB.X;
		}

		return PinPosA.Y < PinPosB.Y;
	};

	for (TArray<UEdGraphPin*>& Pins : OutputInput)
	{
		for (UEdGraphPin* MyPin : Pins)
		{
			TArray<UEdGraphPin*> LinkedPins = MyPin->LinkedTo;

			if (MyPin->Direction == EGPD_Input && GetMutableDefault<UBASettings>()->FormattingStyle == EBANodeFormattingStyle::Expanded)
			{
				LinkedPins.StableSort(LinkedToSorter);
			}

			for (int i = 0; i < LinkedPins.Num(); ++i)
			{
				UEdGraphPin* OtherPin = LinkedPins[i];
				UEdGraphNode* OtherNode = OtherPin->GetOwningNode();
				FPinLink Link(MyPin, OtherPin);

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Iterating %s"), *Link.ToString());

				if (VisitedLinks.Contains(Link)
					|| !NodePool.Contains(OtherNode)
					|| FBAUtils::IsNodePure(OtherNode)
					|| NodesToCollisionCheck.Contains(OtherNode))
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tSkipping"));
					continue;
				}
				VisitedLinks.Add(Link);

				if (bFirstPin && (ParentPin == nullptr || MyPin->Direction == ParentPin->Direction))
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("Same row?"));
					SameRowMapping.Add(Link, true);
					SameRowMapping.Add(FPinLink(OtherPin, MyPin), true);
					bFirstPin = false;
				}

				GetPinsOfSameHeight_Recursive(OtherNode, OtherPin, MyPin, NodesToCollisionCheck, VisitedLinks);
			}
		}
	}
}

void FEdGraphFormatter::AddKnotNodesToComments()
{
	if (CommentNodesContains.Num() == 0)
	{
		return;
	}

	for (TSharedPtr<FKnotNodeTrack> Track : KnotTracks)
	{
		TArray<UEdGraphNode*> TrackNodes = Track->GetNodes(GraphHandler->GetFocusedEdGraph()).Array();

		int NumKnots = 0;
		UK2Node_Knot* SingleKnot = nullptr;
		for (auto Creation : Track->KnotCreations)
		{
			if (Creation->CreatedKnot != nullptr)
			{
				NumKnots += 1;
				SingleKnot = Creation->CreatedKnot;
			}
		}

		for (const auto& Elem : CommentNodesContains)
		{
			UEdGraphNode_Comment* Comment = Elem.Key;
			TArray<UEdGraphNode*> Containing = Elem.Value;
			FSlateRect CommentBounds = GetCommentBounds(Comment, nullptr); // .ExtendBy(30);

			const auto IsNotInsideComment = [&CommentBounds](TSharedPtr<FKnotNodeCreation> Creation)
			{
				UK2Node_Knot* Knot = Creation->CreatedKnot;

				if (Knot != nullptr)
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tComment bounds %s | %s"), *CommentBounds.ToString(), *FVector2D(Knot->NodePosX, Knot->NodePosY).ToString());
					return !CommentBounds.ContainsPoint(FVector2D(Knot->NodePosX, Knot->NodePosY));
				}

				return false;
			};

			bool bContainsSingleKnot = NumKnots == 1 && CommentBounds.ContainsPoint(FVector2D(SingleKnot->NodePosX, SingleKnot->NodePosY));
			const bool bContainsAllNodes = FBAUtils::DoesArrayContainsAllItems(Containing, TrackNodes);

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tAllKnots %d | AllNodes %d"), bContainsAllKnots, bContainsAllNodes);

			if (bContainsAllNodes)
			{
				if (!(NumKnots == 1 && bContainsSingleKnot))
				{
					auto NodesUnderComment = Comment->GetNodesUnderComment();
					for (auto Creation : Track->KnotCreations)
					{
						if (!NodesUnderComment.Contains(Creation->CreatedKnot))
						{
							Comment->AddNodeUnderComment(Creation->CreatedKnot);
						}
					}
				}
			}
		}
	}
}

UEdGraphNode* FEdGraphFormatter::GetHighestLevelParentNode(UEdGraphNode* Node)
{
	while (FormatXInfoMap.Contains(Node))
	{
		auto Info = FormatXInfoMap[Node];
		if (NodeHeightLevels[Node] == 0)
		{
			return Node;
		}

		return GetHighestLevelParentNode(Info->Parent->GetNode());
	}

	return nullptr;
}

void FEdGraphFormatter::WrapNodes()
{
	TArray<UEdGraphNode*> PendingNodes;
	PendingNodes.Push(RootNode);

	TSet<UEdGraphNode*> VisitedNodes;

	const float RootPos = RootNode->NodePosX;

	while (PendingNodes.Num() > 0)
	{
		UEdGraphNode* NextNode = PendingNodes.Pop();
		if (NextNode->NodePosX - RootPos > 1000)
		{
			TSharedPtr<FFormatXInfo> Info = FormatXInfoMap[NextNode];
			TArray<UEdGraphNode*> Children = Info->GetChildren(EGPD_Output);

			float Offset = RootPos - NextNode->NodePosX;
			NextNode->NodePosX += Offset;
			NextNode->NodePosY += 500;

			for (UEdGraphNode* Child : Children)
			{
				Child->NodePosX += Offset;
				Child->NodePosY += 500;
			}
		}

		TArray<UEdGraphNode*> OutputNodes = FBAUtils::GetLinkedNodes(NextNode, EGPD_Output);

		for (UEdGraphNode* Node : OutputNodes)
		{
			if (VisitedNodes.Contains(Node))
			{
				continue;
			}

			VisitedNodes.Add(Node);
			PendingNodes.Add(Node);
		}
	}
}

void FEdGraphFormatter::PrintKnotTracks()
{
	UE_LOG(LogBlueprintAssist, Warning, TEXT("### All Knot Tracks"));
	for (TSharedPtr<FKnotNodeTrack> Track : KnotTracks)
	{
		FString Aligned = Track->GetPinToAlignTo() != nullptr ? FString("True") : FString("False");
		FString Looping = Track->bIsLoopingTrack ? FString("True") : FString("False");
		UE_LOG(LogBlueprintAssist, Warning, TEXT("\tKnot Tracks (%d) %s | %s | %s | %s | Aligned %s (%s) | Looping %s"),
				Track->KnotCreations.Num(),
				*FBAUtils::GetPinName(Track->ParentPin),
				*FBAUtils::GetNodeName(Track->ParentPin->GetOwningNodeUnchecked()),
				*FBAUtils::GetPinName(Track->GetLastPin()),
				*FBAUtils::GetNodeName(Track->GetLastPin()->GetOwningNodeUnchecked()),
				*Aligned, *FBAUtils::GetPinName(Track->GetPinToAlignTo()),
				*Looping);

		for (TSharedPtr<FKnotNodeCreation> Elem : Track->KnotCreations)
		{
			if (auto MyPin = FBAUtils::GetPinFromGraph(Elem->PinToConnectToHandle, GraphHandler->GetFocusedEdGraph()))
			{
				UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t%s %s"), *FBAUtils::GetPinName(MyPin), *Elem->KnotPos.ToString());
			}

			for (auto PinHandle : Elem->PinHandlesToConnectTo)
			{
				if (auto MyPin = FBAUtils::GetPinFromGraph(PinHandle, GraphHandler->GetFocusedEdGraph()))
				{
					UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t%s"), *FBAUtils::GetPinName(MyPin));
				}
			}
		}
	}
}

void FEdGraphFormatter::ExpandKnotTracks()
{
	// UE_LOG(LogBlueprintAssist, Error, TEXT("### Expanding Knot Tracks"));
	// for (auto Elem : KnotTracks)
	// {
	// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("KnotTrack %s"), *FBAUtils::GetPinName(Elem->GetParentPin()));
	// }

	// sort tracks by:
	// 1. exec over parameter
	// 2. Highest track Y
	// 3. Smallest track width
	// 4. Parent pin height
	TSharedPtr<FBAGraphHandler> GraphHandlerCapture = GraphHandler;
	const auto& ExpandTrackSorter = [GraphHandlerCapture](const TSharedPtr<FKnotNodeTrack>& TrackA, const TSharedPtr<FKnotNodeTrack>& TrackB)
	{
		const bool bIsExecPinA = FBAUtils::IsExecPin(TrackA->GetLastPin());
		const bool bIsExecPinB = FBAUtils::IsExecPin(TrackB->GetLastPin());
		if (bIsExecPinA != bIsExecPinB)
		{
			return bIsExecPinA > bIsExecPinB;
		}

		if (bIsExecPinA && TrackA->bIsLoopingTrack != TrackB->bIsLoopingTrack)
		{
			return TrackA->bIsLoopingTrack < TrackB->bIsLoopingTrack;
		}

		if (TrackA->GetTrackHeight() != TrackB->GetTrackHeight())
		{
			return TrackA->bIsLoopingTrack
				? TrackA->GetTrackHeight() > TrackB->GetTrackHeight()
				: TrackA->GetTrackHeight() < TrackB->GetTrackHeight();
		}

		const float WidthA = TrackA->GetTrackBounds().GetSize().X;
		const float WidthB = TrackB->GetTrackBounds().GetSize().X;
		if (WidthA != WidthB)
		{
			return TrackA->bIsLoopingTrack
				? WidthA > WidthB
				: WidthA < WidthB;
		}

		return GraphHandlerCapture->GetPinY(TrackA->GetLastPin()) < GraphHandlerCapture->GetPinY(TrackB->GetLastPin());
	};

	const auto& OverlappingTrackSorter = [GraphHandlerCapture](const TSharedPtr<FKnotNodeTrack>& TrackA, const TSharedPtr<FKnotNodeTrack>& TrackB)
	{
		if (TrackA->bIsLoopingTrack != TrackB->bIsLoopingTrack)
		{
			return TrackA->bIsLoopingTrack < TrackB->bIsLoopingTrack;
		}

		const bool bIsExecPinA = FBAUtils::IsExecPin(TrackA->GetLastPin());
		const bool bIsExecPinB = FBAUtils::IsExecPin(TrackB->GetLastPin());
		if (bIsExecPinA != bIsExecPinB)
		{
			return bIsExecPinA > bIsExecPinB;
		}

		const float WidthA = TrackA->GetTrackBounds().GetSize().X;
		const float WidthB = TrackB->GetTrackBounds().GetSize().X;
		if (WidthA != WidthB)
		{
			return TrackA->bIsLoopingTrack
				? WidthA > WidthB
				: WidthA < WidthB;
		}

		return GraphHandlerCapture->GetPinY(TrackA->GetLastPin()) < GraphHandlerCapture->GetPinY(TrackB->GetLastPin());
	};

	TArray<TSharedPtr<FKnotNodeTrack>> SortedTracks = KnotTracks;
	SortedTracks.StableSort(ExpandTrackSorter);

	TArray<TSharedPtr<FKnotNodeTrack>> PendingTracks = SortedTracks;

	// for (auto Track : SortedTracks)
	// {
	// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("Expanding tracks %s"), *Track->ToString());
	// }

	TSet<TSharedPtr<FKnotNodeTrack>> PlacedTracks;
	while (PendingTracks.Num() > 0)
	{
		TSharedPtr<FKnotNodeTrack> CurrentTrack = PendingTracks[0];
		PlacedTracks.Add(CurrentTrack);

		const float TrackY = CurrentTrack->GetTrackHeight();

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Process pending Track %s (%s)"), *FBAUtils::GetPinName(CurrentTrack->ParentPin), *FBAUtils::GetNodeName(CurrentTrack->ParentPin->GetOwningNode()));

		// check against all other tracks, and find ones which overlap with the current track
		TArray<TSharedPtr<FKnotNodeTrack>> OverlappingTracks;
		OverlappingTracks.Add(CurrentTrack);

		float CurrentLowestTrackHeight = CurrentTrack->GetTrackHeight();
		FSlateRect OverlappingBounds = CurrentTrack->GetTrackBounds();
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Current Track bounds %s"), *CurrentTrack->GetTrackBounds().ToString());
		bool bFoundCollision = true;
		do
		{
			bFoundCollision = false;
			for (TSharedPtr<FKnotNodeTrack> Track : SortedTracks)
			{
				if (OverlappingTracks.Contains(Track))
				{
					continue;
				}

				FSlateRect TrackBounds = Track->GetTrackBounds();

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tOverlapping Bounds %s | %s"), *OverlappingBounds.ToString(), *TrackBounds.ToString());
				if (FSlateRect::DoRectanglesIntersect(OverlappingBounds, TrackBounds))
				{
					OverlappingTracks.Add(Track);
					PlacedTracks.Add(Track);
					bFoundCollision = true;

					OverlappingBounds.Top = FMath::Min(Track->GetTrackHeight(), OverlappingBounds.Top);
					OverlappingBounds.Left = FMath::Min(TrackBounds.Left, OverlappingBounds.Left);
					OverlappingBounds.Right = FMath::Max(TrackBounds.Right, OverlappingBounds.Right);
					OverlappingBounds.Bottom = OverlappingBounds.Top + (OverlappingTracks.Num() * TrackSpacing);

					if (CurrentTrack->HasPinToAlignTo())
					{
						CurrentTrack->PinToAlignTo = nullptr;
					}

					if (Track->HasPinToAlignTo())
					{
						// UE_LOG(LogBlueprintAssist, Warning, TEXT("Removed pin to align to for %s"), *Track->ToString());
						Track->PinToAlignTo = nullptr;
					}

					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tTrack %s colliding %s"), *FBAUtils::GetPinName(Track->ParentPin), *FBAUtils::GetPinName(CurrentTrack->ParentPin));
				}
			}
		}
		while (bFoundCollision);

		if (OverlappingTracks.Num() == 1)
		{
			PendingTracks.Remove(CurrentTrack);
			continue;
		}

		bool bOverlappingLoopingTrack = false;
		TArray<TSharedPtr<FKnotNodeTrack>> ExecTracks;

		// Group overlapping tracks by node (expect for exec tracks)
		TArray<FGroupedTracks> OverlappingGroupedTracks;
		for (TSharedPtr<FKnotNodeTrack> Track : OverlappingTracks)
		{
			if (FBAUtils::IsExecPin(Track->ParentPin) && !Track->bIsLoopingTrack)
			{
				ExecTracks.Add(Track);
				continue;
			}

			if (!Track->bIsLoopingTrack)
			{
				bOverlappingLoopingTrack = true;
			}

			const auto MatchesNode = [&Track](const FGroupedTracks& OtherTrack)
			{
				return Track->ParentPin->GetOwningNode() == OtherTrack.ParentNode;
			};

			FGroupedTracks* Group = OverlappingGroupedTracks.FindByPredicate(MatchesNode);
			if (Group)
			{
				Group->Tracks.Add(Track);
			}
			else
			{
				FGroupedTracks NewGroup;
				NewGroup.ParentNode = Track->ParentPin->GetOwningNode();
				NewGroup.Tracks.Add(Track);
				OverlappingGroupedTracks.Add(NewGroup);
			}
		}

		ExecTracks.StableSort(OverlappingTrackSorter);

		for (auto& Group : OverlappingGroupedTracks)
		{
			Group.Init();
			Group.Tracks.StableSort(OverlappingTrackSorter);
		}

		const auto& GroupSorter = [](const FGroupedTracks& GroupA, const FGroupedTracks& GroupB)
		{
			if (GroupA.bLooping != GroupB.bLooping)
			{
				return GroupA.bLooping < GroupB.bLooping;
			}

			return GroupA.Width < GroupB.Width;
		};
		OverlappingGroupedTracks.StableSort(GroupSorter);

		int TrackCount = 0;
		for (auto Track : ExecTracks)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tTrack %s"), *Track->ToString());
			Track->UpdateTrackHeight(CurrentLowestTrackHeight + (TrackCount * TrackSpacing));
			TrackCount += 1;
		}

		for (FGroupedTracks& Group : OverlappingGroupedTracks)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Group %s"), *FBAUtils::GetNodeName(Group.ParentNode));
			for (TSharedPtr<FKnotNodeTrack> Track : Group.Tracks)
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tTrack %s"), *Track->ToString());
				Track->UpdateTrackHeight(CurrentLowestTrackHeight + (TrackCount * TrackSpacing));
				TrackCount += 1;
			}
		}

		for (TSharedPtr<FKnotNodeTrack> Track : PlacedTracks)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tPlaced track %s"), *Track->ToString());
			PendingTracks.Remove(Track);
		}

		TSet<UEdGraphNode*> TrackNodes = CurrentTrack->GetNodes(GraphHandler->GetFocusedEdGraph());

		FSlateRect ExpandedBounds = OverlappingBounds;
		const float Padding = bOverlappingLoopingTrack ? TrackSpacing * 2 : TrackSpacing;
		ExpandedBounds.Bottom += Padding;

		// find the top of the tallest node the track block is colliding with
		bool bAnyCollision = false;
		float CollisionTop = MAX_flt;

		// collide against nodes
		for (UEdGraphNode* Node : GetFormattedGraphNodes())
		{
			// if (Node == CurrentTrack->LinkedTo[0]->GetOwningNode() || Node == CurrentTrack->GetLastPin()->GetOwningNode())
			// 	continue;

			bool bSkipNode = false;
			for (TSharedPtr<FKnotNodeTrack> Track : PlacedTracks)
			{
				if (Node == Track->ParentPin->GetOwningNode() || Node == Track->GetLastPin()->GetOwningNode())
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("Skipping node %s"), *FBAUtils::GetNodeName(Node));
					bSkipNode = true;
					break;
				}

				if (auto AlignedPin = Track->GetPinToAlignTo())
				{
					if (Node == AlignedPin->GetOwningNode())
					{
						// UE_LOG(LogBlueprintAssist, Warning, TEXT("Skipping node aligned %s"), *FBAUtils::GetNodeName(Node));
						bSkipNode = true;
						break;
					}
				}
			}

			if (bSkipNode)
			{
				continue;
			}

			const FSlateRect NodeBounds = GraphHandler->GetCachedNodeBounds(Node);
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Checking collision for %s | %s | %s"), *FBAUtils::GetNodeName(Node), *NodeBounds.ToString(), *ExpandedBounds.ToString());

			if (FSlateRect::DoRectanglesIntersect(NodeBounds, ExpandedBounds))
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Collision with %s"), *FBAUtils::GetNodeName(Node));
				bAnyCollision = true;
				CollisionTop = FMath::Min(NodeBounds.Top, CollisionTop);
			}
		}

		if (!bAnyCollision)
		{
			continue;
		}

		if (GetDefault<UBASettings>()->bCustomDebug == 200)
		{
			continue;
		}

		float Delta = ExpandedBounds.Bottom - CollisionTop;
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("### Moving nodes down | Delta %f"), Delta);

		// move all nodes below the track block
		TSet<UEdGraphNode*> MovedNodes;
		for (UEdGraphNode* Node : GetFormattedGraphNodes())
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tChecking Node for collision %s | My %d | Track %f"), *FBAUtils::GetNodeName(Node), Node->NodePosY, TrackY);

			if (Node->NodePosY > TrackY)
			{
				Node->NodePosY += Delta;
				MovedNodes.Add(Node);
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t Moved node %s by delta %f"), *FBAUtils::GetNodeName(Node), Delta);
			}
		}

		// Update other tracks since their nodes may have moved
		for (TSharedPtr<FKnotNodeTrack> Track : SortedTracks)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("CHECKING track %s"), *Track->ToString());
			if (PlacedTracks.Contains(Track))
			{
				continue;
			}

			if (Track->HasPinToAlignTo()) // if we are aligned to a pin, update our track y when a node moves
			{
				if (MovedNodes.Contains(Track->GetLastPin()->GetOwningNode()) || MovedNodes.Contains(Track->ParentPin->GetOwningNode()))
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tMoved Aligned Track for node delta %s"), *FBAUtils::GetNodeName(Track->GetLastPin()->GetOwningNode()));
					Track->UpdateTrackHeight(Track->GetTrackHeight() + Delta);
				}
			}
			else if (Track->GetTrackHeight() > TrackY)
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tMoved BELOW Track for node delta %s"), *FBAUtils::GetNodeName(Track->GetLastPin()->GetOwningNode()));
				Track->UpdateTrackHeight(Track->GetTrackHeight() + Delta);
			}
		}
	}
}

void FEdGraphFormatter::RemoveUselessCreationNodes()
{
	for (TSharedPtr<FKnotNodeTrack> Track : KnotTracks)
	{
		TSharedPtr<FKnotNodeCreation> LastCreation;
		TArray<TSharedPtr<FKnotNodeCreation>> CreationsCopy = Track->KnotCreations;
		for (TSharedPtr<FKnotNodeCreation> Creation : CreationsCopy)
		{
			const bool bHasOneConnection = Creation->PinHandlesToConnectTo.Num() == 1;
			if (bHasOneConnection)
			{
				const float PinHeight = GraphHandler->GetPinY(Creation->GetPinToConnectTo());
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Pin %s %f | %f"), *FBAUtils::GetPinName(MainPin), PinHeight, Track->GetTrackHeight());

				if (PinHeight == Track->GetTrackHeight())
				{
					if (LastCreation)
					{
						LastCreation->PinHandlesToConnectTo.Add(Creation->PinToConnectToHandle);
					}

					Track->KnotCreations.Remove(Creation);
				}
			}

			LastCreation = Creation;
		}
	}
}

void FEdGraphFormatter::FormatKnotNodes()
{
	//UE_LOG(LogBlueprintAssist, Warning, TEXT("### Format Knot Nodes"));

	MakeKnotTrack();

	MergeNearbyKnotTracks();

	ExpandKnotTracks();

	RemoveUselessCreationNodes();

	CreateKnotTracks();

	if (GetDefault<UBASettings>()->bAddKnotNodesToComments)
	{
		AddKnotNodesToComments();
	}
}

void FEdGraphFormatter::CreateKnotTracks()
{
	// we sort tracks by
	// 1. exec pin track over parameter track 
	// 2. top-most-track-height 
	// 3. top-most parent pin 
	// 4. left-most
	const auto& TrackSorter = [](const TSharedPtr<FKnotNodeTrack> TrackA, const TSharedPtr<FKnotNodeTrack> TrackB)
	{
		const bool bIsExecPinA = FBAUtils::IsExecPin(TrackA->GetLastPin());
		const bool bIsExecPinB = FBAUtils::IsExecPin(TrackB->GetLastPin());

		if (bIsExecPinA != bIsExecPinB)
		{
			return bIsExecPinA > bIsExecPinB;
		}

		if (TrackA->GetTrackHeight() != TrackB->GetTrackHeight())
		{
			return TrackA->GetTrackHeight() < TrackB->GetTrackHeight();
		}

		if (TrackA->ParentPinPos.Y != TrackB->ParentPinPos.Y)
		{
			return TrackA->ParentPinPos.Y < TrackB->ParentPinPos.Y;
		}

		return TrackA->GetTrackBounds().GetSize().X < TrackB->GetTrackBounds().GetSize().X;
	};
	KnotTracks.Sort(TrackSorter);

	for (TSharedPtr<FKnotNodeTrack> KnotTrack : KnotTracks)
	{
		// sort knot creations
		auto GraphCapture = GraphHandler->GetFocusedEdGraph();
		const auto CreationSorter = [GraphCapture](TSharedPtr<FKnotNodeCreation> CreationA, TSharedPtr<FKnotNodeCreation> CreationB)
		{
			UEdGraphPin* Pin = FBAUtils::GetPinFromGraph(CreationA->PinToConnectToHandle, GraphCapture);

			if (FBAUtils::IsExecPin(Pin))
			{
				return CreationA->KnotPos.X > CreationB->KnotPos.X;
			}

			return CreationA->KnotPos.X < CreationB->KnotPos.X;
		};

		if (!KnotTrack->bIsLoopingTrack)
		{
			KnotTrack->KnotCreations.StableSort(CreationSorter);
		}

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Creating knot track %s"), *FBAUtils::GetPinName(KnotTrack->ParentPin));
		// if (KnotTrack->PinToAlignTo.IsValid())
		// {
		// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\tShould make aligned track!"));
		// }

		TSharedPtr<FKnotNodeCreation> LastCreation = nullptr;
		const int NumCreations = KnotTrack->KnotCreations.Num();
		for (int i = 0; i < NumCreations; i++)
		{
			TSharedPtr<FKnotNodeCreation> Creation = KnotTrack->KnotCreations[i];

			FVector2D KnotPos = Creation->KnotPos;

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Making knot creation at %s %d"), *KnotPos.ToString(), i);
			// for (auto Pin : Creation->PinHandlesToConnectTo)
			// {
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tPin %s"), *FBAUtils::GetPinName(FBAUtils::GetPinFromGraph(Pin, GraphHandler->GetFocusedEdGraph())));
			// }

			UEdGraphPin* PinToAlignTo = KnotTrack->GetPinToAlignTo();

			if (PinToAlignTo != nullptr)
			{
				KnotPos.Y = GraphHandler->GetPinY(PinToAlignTo);
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Created knot aligned to %s"), *FBAUtils::GetNodeName(PinToAlignTo->GetOwningNode()));
			}

			if (!LastCreation.IsValid()) // create a knot linked to the first pin (the fallback pin)
			{
				UEdGraphPin* ParentPin = FBAUtils::GetPinFromGraph(FGraphPinHandle(KnotTrack->ParentPin), GraphHandler->GetFocusedEdGraph());
				UK2Node_Knot* KnotNode = CreateKnotNode(Creation.Get(), KnotPos, ParentPin);
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Create initial %s"), *FBAUtils::GetPinName(ParentPin));

				KnotNodesSet.Add(KnotNode);
				LastCreation = Creation;
			}
			else // create a knot that connects to the last knot
			{
				UK2Node_Knot* ParentKnot = LastCreation->CreatedKnot;

				const bool bCreatePinAlignedKnot = LastCreation->PinHandlesToConnectTo.Num() == 1 && PinToAlignTo != nullptr;
				if (bCreatePinAlignedKnot && NumCreations == 1) // move the parent knot to the aligned x position
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("Create pin aligned!"));
					for (const FGraphPinHandle& PinHandle : Creation->PinHandlesToConnectTo)
					{
						UEdGraphPin* Pin = FBAUtils::GetPinFromGraph(PinHandle, GraphHandler->GetFocusedEdGraph());

						UEdGraphPin* ParentPin = Pin->Direction == EGPD_Input
							? ParentKnot->GetOutputPin()
							: ParentKnot->GetInputPin();
						FBAUtils::TryCreateConnection(ParentPin, Pin);
					}
				}
				else
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("Create normal"));
					UEdGraphPin* LastPin = KnotTrack->GetLastPin();

					UEdGraphPin* PinOnLastKnot = LastPin->Direction == EGPD_Output
						? ParentKnot->GetInputPin()
						: ParentKnot->GetOutputPin();

					UK2Node_Knot* NewKnot = CreateKnotNode(Creation.Get(), KnotPos, PinOnLastKnot);
					KnotNodesSet.Add(NewKnot);

					LastCreation = Creation;
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(GraphHandler->GetBlueprint());
}

void FEdGraphFormatter::ResetRelativeToNodeToKeepStill(const FVector2D& SavedLocation)
{
	const float DeltaX = SavedLocation.X - NodeToKeepStill->NodePosX;
	const float DeltaY = SavedLocation.Y - NodeToKeepStill->NodePosY;

	if (DeltaX != 0 || DeltaY != 0)
	{
		TSet<UEdGraphNode*> AllNodes = GetFormattedGraphNodes();
		for (UEdGraphNode* Node : AllNodes)
		{
			Node->NodePosX += DeltaX;
			Node->NodePosY += DeltaY;
		}

		for (UEdGraphNode* Node : KnotNodesSet)
		{
			Node->NodePosX += DeltaX;
			Node->NodePosY += DeltaY;
		}
	}
}

void FEdGraphFormatter::FormatComments()
{
	TSet<UEdGraphNode_Comment*> FormattedComments;
	TSet<FPinLink> VisitedLinks;
	TSet<UEdGraphNode*> VisitedNodes;
	TArray<FPinLink> OutputStack;
	TArray<FPinLink> InputStack;
	const FPinLink RootNodeLink(nullptr, nullptr, RootNode);
	OutputStack.Push(RootNodeLink);

	EEdGraphPinDirection LastDirection = EGPD_Output;

	NodesToExpand.Reset();

	while (OutputStack.Num() > 0 || InputStack.Num() > 0)
	{
		// try to get the current info from the pending input
		FPinLink CurrentLink;

		TArray<FPinLink>& FirstStack = LastDirection == EGPD_Output ? OutputStack : InputStack;
		TArray<FPinLink>& SecondStack = LastDirection == EGPD_Output ? InputStack : OutputStack;

		if (FirstStack.Num() > 0)
		{
			CurrentLink = FirstStack.Pop();
		}
		else
		{
			CurrentLink = SecondStack.Pop();
		}

		LastDirection = CurrentLink.GetDirection();

		UEdGraphNode* CurrentNode = CurrentLink.GetNode();
		VisitedNodes.Add(CurrentNode);

		// Format the comment sub graph
		if (ParentComments.Contains(CurrentNode))
		{
			TArray<UEdGraphNode_Comment*>& Comments = ParentComments[CurrentNode];
			for (auto Comment : Comments)
			{
				if (FormattedComments.Contains(Comment))
				{
					continue;
				}

				FormattedComments.Add(Comment);

				FCommentSubGraphFormatterParameters SubGraphParameters;
				SubGraphParameters.bIsCommentFormatter = true;
				SubGraphParameters.NodesToFormat = CommentNodesContains[Comment];

				if (FBAUtils::DoesArrayContainsAllItems(SubGraphParameters.NodesToFormat, NodeTree))
				{
					continue;
				}

				TSharedPtr<FCommentSubGraphFormatter> SubGraphFormatter = MakeShared<FCommentSubGraphFormatter>(GraphHandler, SubGraphParameters, SharedThis(this));

				SubGraphFormatter->FormatNode(CurrentNode);

				SubGraphFormatters.Add(CurrentNode, SubGraphFormatter);
			}
		}

		TArray<UEdGraphPin*> LinkedPins = FBAUtils::GetLinkedPins(CurrentNode).FilterByPredicate(IsExecOrDelegatePin);

		for (int i = LinkedPins.Num() - 1; i >= 0; --i)
		{
			UEdGraphPin* ParentPin = LinkedPins[i];

			for (UEdGraphPin* LinkedPin : ParentPin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();

				const FPinLink PinLink(ParentPin, LinkedPin, LinkedNode);
				if (VisitedLinks.Contains(PinLink))
				{
					continue;
				}

				VisitedLinks.Add(PinLink);
				if (!NodePool.Contains(LinkedNode))
				{
					continue;
				}

				if (FBAUtils::IsNodePure(LinkedNode))
				{
					continue;
				}

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tIterating pin link %s"), *PinLink.ToString());

				if (ParentPin->Direction == EGPD_Output)
				{
					OutputStack.Push(PinLink);
				}
				else
				{
					InputStack.Push(PinLink);
				}
			}
		}
	}
}

int32 FEdGraphFormatter::GetChildX(const FPinLink& Link, const bool bUseClusterNodes)
{
	if (Link.From == nullptr)
	{
		return GetRelativeNodeBounds(Link.GetNode(), nullptr, bUseClusterNodes).Left;
	}

	float NewNodePos;
	UEdGraphNode* Parent = Link.From->GetOwningNode();
	UEdGraphNode* Child = Link.To->GetOwningNode();
	FSlateRect ParentBounds = bUseClusterNodes
		? GetClusterBounds(Parent)
		: FBAUtils::GetCachedNodeBounds(GraphHandler, Parent);

	// if (auto SameRowPtr = SameRowMapping.Find(Link))
	{
		// if (*SameRowPtr)
		{
			TSet<UEdGraphNode*> RelativeNodes;
			auto RelativeBounds = GetRelativeNodeBounds(Parent, Child, RelativeNodes, bUseClusterNodes);

			auto SameRowPtr = SameRowMapping.Find(Link);
			if (SameRowPtr != nullptr && *SameRowPtr)
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t\t\tTaking Relative bounds %s | OLD %s"), *RelativeBounds.ToString(), *ParentBounds.ToString());
				ParentBounds = RelativeBounds;
			}
			else
			{
				const auto NodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, RelativeNodes.Array());

				bool bIsSame = Link.GetDirection() == EGPD_Output ? NodesBounds.Right == ParentBounds.Right : NodesBounds.Left == ParentBounds.Left;

				if (bIsSame)
				{
					// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\t\t\tTaking Relative bounds %s | OLD %s"), *RelativeBounds.ToString(), *ParentBounds.ToString());

					ParentBounds = RelativeBounds;
				}
			}

			//ParentBounds = GetRelativeNodeBounds(Parent, Child, bUseClusterNodes);
		}
	}

	FSlateRect ChildBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, Child);

	FSlateRect LargerBounds = GetRelativeNodeBounds(Child, Parent, bUseClusterNodes);

	if (Link.From->Direction == EGPD_Input)
	{
		const float Delta = LargerBounds.Right - ChildBounds.Left;
		NewNodePos = ParentBounds.Left - Delta - NodePadding.X; // -1;
	}
	else
	{
		const float Delta = ChildBounds.Left - LargerBounds.Left;
		NewNodePos = ParentBounds.Right + Delta + NodePadding.X; // +1;
	}

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Node %s New Node Pos %d | %s | %s | %s"), *Link.ToString(), FMath::RoundToInt(NewNodePos), *ParentBounds.ToString(), *ChildBounds.ToString(), *LargerBounds.ToString());

	return FMath::RoundToInt(NewNodePos);
}

void FEdGraphFormatter::RemoveKnotNodes()
{
	TSet<UEdGraphNode*> RemovedNodes;

	auto& GraphHandlerCapture = GraphHandler;
	auto& FormatterParamsCapture = FormatterParameters;
	const auto OnlySelected = [&GraphHandlerCapture, &FormatterParamsCapture](UEdGraphPin* Pin)
	{
		return GraphHandlerCapture->FilterSelectiveFormatting(Pin->GetOwningNode(), FormatterParamsCapture.NodesToFormat)
			&& (FBAUtils::IsParameterPin(Pin) || IsExecOrDelegatePin(Pin));
	};

	TArray<UEdGraphNode_Comment*> CommentNodes = FBAUtils::GetCommentNodesFromGraph(GraphHandler->GetFocusedEdGraph());

	for (UEdGraphNode* Node : FBAUtils::GetNodeTreeWithFilter(RootNode, OnlySelected))
	{
		/** Delete all connections for each knot node */
		if (UK2Node_Knot* KnotNode = Cast<UK2Node_Knot>(Node))
		{
			FBAUtils::DisconnectKnotNode(KnotNode);

			for (auto Comment : CommentNodes)
			{
				if (Comment->GetNodesUnderComment().Contains(KnotNode))
				{
					FBAUtils::RemoveNodeFromComment(Comment, KnotNode);
				}
			}

			if (GetMutableDefault<UBASettings>()->bUseKnotNodePool)
			{
				KnotNodePool.Add(KnotNode);
			}
			else
			{
				FBAUtils::DeleteNode(KnotNode);
			}
		}
	}
}

bool FEdGraphFormatter::IsExecOrDelegatePin(UEdGraphPin* Pin)
{
	const bool bUseDelegatePins = GetMutableDefault<UBASettings>()->bTreatDelegatesAsExecutionPins && FBAUtils::IsDelegatePin(Pin) && FBAUtils::IsNodeImpure(Pin->GetOwningNode());
	return FBAUtils::IsExecPin(Pin) || bUseDelegatePins;
}

void FEdGraphFormatter::ModifyCommentNodes()
{
	if (!GetMutableDefault<UBASettings>()->bTryToHandleCommentNodes)
	{
		return;
	}

	/** Modify all comment nodes which our formatted nodes (needed for the AutoSizeComment plugin) */
	TArray<UEdGraphNode_Comment*> AllCommentNodes;
	GraphHandler->GetFocusedEdGraph()->GetNodesOfClass(AllCommentNodes);

	for (UEdGraphNode_Comment* CommentNode : AllCommentNodes)
	{
		TArray<UEdGraphNode*> EdGraphNodesUnderComment;

		bool bShouldUpdateNode = false;
		for (UObject* NodeUnderComment : CommentNode->GetNodesUnderComment())
		{
			if (UEdGraphNode* Node = Cast<UEdGraphNode>(NodeUnderComment))
			{
				if (!FBAUtils::IsCommentNode(Node) && !FBAUtils::IsKnotNode(Node))
				{
					if (GetFormattedGraphNodes().Contains(Node))
					{
						bShouldUpdateNode = true;
					}

					EdGraphNodesUnderComment.Add(Node);
				}
			}
		}

		if (bShouldUpdateNode)
		{
			TSharedPtr<SGraphNodeComment> GraphNodeComment = StaticCastSharedPtr<SGraphNodeComment>(FBAUtils::GetGraphNode(GraphHandler->GetGraphPanel(), CommentNode));
			if (GraphNodeComment.IsValid())
			{
				const float TitlebarHeight = GraphNodeComment->GetDesiredSizeForMarquee().Y;
				const FVector2D CommentPadding = GetDefault<UBASettings>()->CommentNodePadding;
				const FMargin Padding = FMargin(CommentPadding.X, CommentPadding.Y + TitlebarHeight, CommentPadding.X, CommentPadding.Y);
				const FSlateRect NewBounds = FBAUtils::GetNodeArrayBounds(EdGraphNodesUnderComment).ExtendBy(Padding);
				CommentNode->SetBounds(NewBounds);
			}
		}
	}
}

UK2Node_Knot* FEdGraphFormatter::CreateKnotNode(FKnotNodeCreation* Creation, const FVector2D& Position, UEdGraphPin* ParentPin)
{
	if (!Creation)
	{
		return nullptr;
	}

	UK2Node_Knot* OptionalNodeToReuse = nullptr;
	if (GetMutableDefault<UBASettings>()->bUseKnotNodePool && KnotNodePool.Num() > 0)
	{
		OptionalNodeToReuse = KnotNodePool.Pop();
	}
	else
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Failed to find?"));
	}

	auto Graph = GraphHandler->GetFocusedEdGraph();
	UK2Node_Knot* CreatedNode = Creation->CreateKnotNode(Position, ParentPin, OptionalNodeToReuse, Graph);

	UEdGraphPin* MainPinToConnectTo = FBAUtils::GetPinFromGraph(Creation->PinToConnectToHandle, Graph);

	KnotNodeOwners.Add(CreatedNode, MainPinToConnectTo->GetOwningNode());
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Created node %d for %s"), CreatedNode, *FBAUtils::GetNodeName(ParentPin->GetOwningNode()));

	return CreatedNode; //Creation->CreateKnotNode(Position, ParentPin, OptionalNodeToReuse, GraphHandler->GetFocusedEdGraph());
}

void FEdGraphFormatter::GetPinsOfSameHeight()
{
	TSet<UEdGraphNode*> NodesToCollisionCheck;
	TSet<FPinLink> VisitedLinks;
	TSet<UEdGraphNode*> TempChildren;
	GetPinsOfSameHeight_Recursive(RootNode, nullptr, nullptr, NodesToCollisionCheck, VisitedLinks);
}

FSlateRect FEdGraphFormatter::GetRelativeNodeBounds(UEdGraphNode* Node, UEdGraphNode* NodeAsking, bool bUseClusterBounds)
{
	TSet<UEdGraphNode*> Unused;
	return GetRelativeNodeBounds(Node, NodeAsking, Unused, bUseClusterBounds);
}

FSlateRect FEdGraphFormatter::GetRelativeNodeBounds(UEdGraphNode* Node, UEdGraphNode* NodeAsking, TSet<UEdGraphNode*>& RelativeNodes, bool bUseClusterBounds)
{
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Getting relative bounds between %s | %s"), *FBAUtils::GetNodeName(Node), *FBAUtils::GetNodeName(NodeAsking));
	if (!GetDefault<UBASettings>()->bAccountForCommentsWhenFormatting || !bAccountForComments)
	{
		return bUseClusterBounds ? GetClusterBounds(Node) : FBAUtils::GetCachedNodeBounds(GraphHandler, Node);
	}

	TArray<UEdGraphNode_Comment*> NodeComments;
	if (ParentComments.Contains(Node))
	{
		NodeComments = ParentComments[Node];
	}

	if (NodeComments.Num() == 0)
	{
		return bUseClusterBounds ? GetClusterBounds(Node) : FBAUtils::GetCachedNodeBounds(GraphHandler, Node);
	}

	TArray<UEdGraphNode_Comment*> NodeAskingCommentNodes;
	if (ParentComments.Contains(NodeAsking))
	{
		NodeAskingCommentNodes = ParentComments[NodeAsking];
	}

	// FSlateRect OutBounds = bUseClusterBounds ? GetRelativeClusterBounds(Node, NodeAsking) : FBAUtils::GetCachedNodeBounds(GraphHandler, Node);
	FSlateRect OutBounds = bUseClusterBounds ? GetClusterBounds(Node) : FBAUtils::GetCachedNodeBounds(GraphHandler, Node);

	// apply the comment node padding
	for (UEdGraphNode_Comment* CommentNode : NodeComments)
	{
		if (NodeAskingCommentNodes.Contains(CommentNode))
		{
			continue;
		}

		auto NodesUnderComment = FBAUtils::GetNodesUnderComment(CommentNode);

		if (NodesUnderComment.Num() == 0)
		{
			continue;
		}

		const auto IsUnderComment = [&NodesUnderComment](const FPinLink& PinLink)
		{
			return NodesUnderComment.Contains(PinLink.GetNode());
		};

		const auto CommentNodeTree = FBAUtils::GetNodeTreeWithFilter(NodesUnderComment[0], IsUnderComment);
		const auto& NodeTreeCapture = NodeTree;

		// skip if 1. nodes are not linked 2. any nodes are not in our node tree 3. if pure node: then the parent must be in the comment box
		auto ThisCapture = this;
		auto& ParameterParentMapCapture = ParameterParentMap;
		const auto ShouldSkip = [&CommentNodeTree, &NodeTreeCapture, &ThisCapture, &NodesUnderComment, &ParameterParentMapCapture](UEdGraphNode* Node)
		{
			if (FBAUtils::IsCommentNode(Node) || FBAUtils::IsKnotNode(Node))
			{
				return false;
			}

			// if (FBAUtils::IsNodePure(Node))
			// {
			// 	if (ParameterParentMapCapture.Contains(Node))
			// 	{
			// 		auto ParamFormatter = ParameterParentMapCapture[Node];
			// 		if (ParamFormatter.IsValid())
			// 		{
			// 			UE_LOG(LogBlueprintAssist, Warning, TEXT("Checking param formatter for %s | %s"), *FBAUtils::GetNodeName(Node), *FBAUtils::GetNodeName(ParamFormatter->GetRootNode()));
			// 			if (!NodesUnderComment.Contains(ParamFormatter->GetRootNode()))
			// 			{
			// 				UE_LOG(LogBlueprintAssist, Warning, TEXT("Skipping root node"));
			// 				return true;
			// 			}
			// 		}
			// 	}
			// }

			return !CommentNodeTree.Contains(Node) || !NodeTreeCapture.Contains(Node);
		};

		if (NodesUnderComment.ContainsByPredicate(ShouldSkip))
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Skipping node!"));
			continue;
		}

		// skip if the other node is contained in our comment node
		if (ParentComments.Contains(NodeAsking))
		{
			bool bSkipCommentNode = false;

			for (UEdGraphNode_Comment* OtherComment : ParentComments[NodeAsking])
			{
				auto OtherNodesUnderComment = FBAUtils::GetNodesUnderComment(OtherComment);
				if (CommentNode == OtherComment || NodesUnderComment.Contains(OtherComment) || OtherNodesUnderComment.Contains(CommentNode))
				{
					continue;
				}

				for (UEdGraphNode* OtherNode : OtherNodesUnderComment)
				{
					if (NodesUnderComment.Contains(OtherNode))
					{
						bSkipCommentNode = true;
						break;
					}
				}
				if (bSkipCommentNode)
				{
					break;
				}
			}
			if (bSkipCommentNode)
			{
				continue;
			}
		}

		auto CommentNodeBounds = GetCommentBounds(CommentNode, NodeAsking);

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tExpanding bounds for %s | OLD %s | NEW %s"), *FBAUtils::GetNodeName(Node), *OutBounds.ToString(), *OutBounds.Expand(CommentNodeBounds).ToString());

		OutBounds = OutBounds.Expand(CommentNodeBounds);

		for (auto NodeUnderComment : NodesUnderComment)
		{
			if (!FBAUtils::IsCommentNode(NodeUnderComment))
			{
				RelativeNodes.Add(NodeUnderComment);
			}
		}
	}

	if (!FSlateRect::IsRectangleContained(FSlateRect(-99999, -99999, 99999, 99999), OutBounds))
	{
		UE_LOG(LogBlueprintAssist, Error, TEXT("Calculating relative bounds has failed, returning regular bounds"));
		return bUseClusterBounds ? GetClusterBounds(Node) : FBAUtils::GetCachedNodeBounds(GraphHandler, Node);
	}

	return OutBounds;
}

FSlateRect FEdGraphFormatter::GetCommentNodeBounds(UEdGraphNode_Comment* CommentNode, const FSlateRect& InBounds, FMargin& PostPadding)
{
	auto ObjUnderComment = CommentNode->GetNodesUnderComment();
	TArray<UEdGraphNode*> NodesUnderComment;
	for (auto Obj : ObjUnderComment)
	{
		if (UEdGraphNode* EdNode = Cast<UEdGraphNode>(Obj))
		{
			if (FBAUtils::IsCommentNode(EdNode))
			{
				continue;
			}

			NodesUnderComment.Add(EdNode);
		}
	}

	if (NodesUnderComment.Num() == 0)
	{
		return FSlateRect::FromPointAndExtent(FVector2D(CommentNode->NodePosX, CommentNode->NodePosY), FVector2D(CommentNode->NodeWidth, CommentNode->NodeHeight));
	}

	const auto ContainedNodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesUnderComment);
	auto OutBounds = InBounds;

	const auto BASettings = GetDefault<UBASettings>();
	const FVector2D Padding = BASettings->CommentNodePadding;
	float TitlebarHeight = 0.f;

	TSharedPtr<SGraphNodeComment> GraphNodeComment = StaticCastSharedPtr<SGraphNodeComment>(FBAUtils::GetGraphNode(GraphHandler->GetGraphPanel(), CommentNode));
	if (GraphNodeComment.IsValid())
	{
		TitlebarHeight = GraphNodeComment->GetDesiredSizeForMarquee().Y;
	}

	if (ContainedNodesBounds.Left == InBounds.Left)
	{
		PostPadding.Left += Padding.X;
	}
	else if (ContainedNodesBounds.Left < InBounds.Left)
	{
		OutBounds.Left = ContainedNodesBounds.Left;
		PostPadding.Left = Padding.X;
	}

	if (ContainedNodesBounds.Right == InBounds.Right)
	{
		PostPadding.Right += Padding.X;
	}
	else if (ContainedNodesBounds.Right > InBounds.Right)
	{
		OutBounds.Right = ContainedNodesBounds.Right;
		PostPadding.Right = Padding.X;
	}

	if (ContainedNodesBounds.Top == InBounds.Top)
	{
		PostPadding.Top += Padding.Y + TitlebarHeight;
	}
	else if (ContainedNodesBounds.Top > InBounds.Top)
	{
		OutBounds.Top = ContainedNodesBounds.Top;
		PostPadding.Top = Padding.Y + TitlebarHeight;
	}

	if (ContainedNodesBounds.Bottom == InBounds.Bottom)
	{
		PostPadding.Bottom += Padding.Y;
	}
	else if (ContainedNodesBounds.Bottom < InBounds.Bottom)
	{
		OutBounds.Bottom = ContainedNodesBounds.Bottom;
		PostPadding.Left = Padding.Y;
	}

	return OutBounds;
}

FSlateRect FEdGraphFormatter::GetCommentBounds(UEdGraphNode_Comment* CommentNode, UEdGraphNode* NodeAsking)
{
	auto ObjUnderComment = CommentNode->GetNodesUnderComment();
	TArray<UEdGraphNode*> NodesUnderComment;
	TArray<UEdGraphNode_Comment*> CommentNodesUnderComment;

	for (auto Obj : ObjUnderComment)
	{
		if (UEdGraphNode* EdNode = Cast<UEdGraphNode>(Obj))
		{
			if (auto Comment = Cast<UEdGraphNode_Comment>(EdNode))
			{
				CommentNodesUnderComment.Add(Comment);
			}
			else
			{
				NodesUnderComment.Add(EdNode);
			}
		}
	}

	// for (auto Node : NodesUnderComment)
	// {
	// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t\tNodeUnderComment %s | %s"), *FBAUtils::GetNodeName(Node), *FBAUtils::GetCachedNodeBounds(GraphHandler, Node).ToString());
	// }

	if (NodesUnderComment.Num() == 0 && CommentNodesUnderComment.Num() == 0)
	{
		return FSlateRect::FromPointAndExtent(FVector2D(CommentNode->NodePosX, CommentNode->NodePosY), FVector2D(CommentNode->NodeWidth, CommentNode->NodeHeight));
	}

	auto ContainedNodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesUnderComment);
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t ContainedNodesBounds %s"), *ContainedNodesBounds.ToString());
	for (UEdGraphNode_Comment* CommentUnderComment : CommentNodesUnderComment)
	{
		if (CommentUnderComment->GetNodesUnderComment().Num() == 0)
		{
			continue;
		}

		if (ParentComments.Contains(NodeAsking))
		{
			if (ParentComments[NodeAsking].Contains(CommentUnderComment))
			{
				continue;
			}
		}

		ContainedNodesBounds = ContainedNodesBounds.Expand(GetCommentBounds(CommentUnderComment, NodeAsking));
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t ContainedNodesBounds %s"), *ContainedNodesBounds.ToString());
	}

	const FVector2D Padding = GetDefault<UBASettings>()->CommentNodePadding;

	const float TitlebarHeight = FBAUtils::GetCachedNodeBounds(GraphHandler, CommentNode, false).GetSize().Y;

	const FMargin CommentPadding(
		Padding.X,
		Padding.Y + TitlebarHeight,
		Padding.X,
		Padding.Y);

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tContainedNodeBounds %s | Padding %s"), *ContainedNodesBounds.ToString(), *CommentPadding.GetDesiredSize().ToString());

	ContainedNodesBounds = ContainedNodesBounds.ExtendBy(CommentPadding);

	return ContainedNodesBounds;
}

void FEdGraphFormatter::ApplyCommentPadding(FSlateRect& Bounds, UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	if (!ParentComments.Contains(Node))
	{
		return;
	}

	FSlateRect NodeBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, Node);

	if (Direction == EGPD_Input)
	{
		float Offset = 0;
		for (auto Comment : ParentComments[Node])
		{
			auto ObjUnderComment = Comment->GetNodesUnderComment();
			TArray<UEdGraphNode*> NodesUnderComment;
			for (auto Obj : ObjUnderComment)
			{
				if (UEdGraphNode* EdNode = Cast<UEdGraphNode>(Obj))
				{
					if (FBAUtils::IsCommentNode(EdNode))
					{
						continue;
					}

					NodesUnderComment.Add(EdNode);
				}
			}

			const auto ContainedNodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesUnderComment);
			if (ContainedNodesBounds.Left == NodeBounds.Left)
			{
				Offset += 30;
			}
		}
		Bounds.Left -= Offset;
	}
	else
	{
		float Offset = 0;
		for (auto Comment : ParentComments[Node])
		{
			auto ObjUnderComment = Comment->GetNodesUnderComment();
			TArray<UEdGraphNode*> NodesUnderComment;
			for (auto Obj : ObjUnderComment)
			{
				if (UEdGraphNode* EdNode = Cast<UEdGraphNode>(Obj))
				{
					if (FBAUtils::IsCommentNode(EdNode))
					{
						continue;
					}

					NodesUnderComment.Add(EdNode);
				}
			}

			const auto ContainedNodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesUnderComment);
			if (ContainedNodesBounds.Right == NodeBounds.Right)
			{
				Offset += 30;
			}
		}
		Bounds.Right += Offset;
	}
}

void FEdGraphFormatter::FormatParameterNodes()
{
	TArray<UEdGraphNode*> IgnoredNodes;

	TArray<UEdGraphNode*> NodePoolCopy = NodePool;

	const auto& LeftTopMostSort = [](const UEdGraphNode& NodeA, const UEdGraphNode& NodeB)
	{
		if (NodeA.NodePosX != NodeB.NodePosX)
		{
			return NodeA.NodePosX < NodeB.NodePosX;
		}

		return NodeA.NodePosY < NodeB.NodePosY;
	};
	NodePoolCopy.StableSort(LeftTopMostSort);

	ParameterParentMap.Reset();

	for (UEdGraphNode* MainNode : NodePoolCopy)
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Format parameters for node %s"), *FBAUtils::GetNodeName(MainNode));

		TSharedPtr<FEdGraphParameterFormatter> ParameterFormatter = GetParameterFormatter(MainNode);
		ParameterFormatter->SetIgnoredNodes(IgnoredNodes);
		ParameterFormatter->FormatNode(MainNode);

		// update node -> parameter formatter map
		for (UEdGraphNode* NodeToCheck : ParameterFormatter->GetFormattedNodes())
		{
			if (ParameterParentMap.Contains(NodeToCheck))
			{
				// if the node already has a parent, update the old parent by removing 
				TSharedPtr<FEdGraphParameterFormatter> ParentFormatter = ParameterParentMap[NodeToCheck];
				ParentFormatter->FormattedOutputNodes.Remove(NodeToCheck);
				ParentFormatter->AllFormattedNodes.Remove(NodeToCheck);
				ParentFormatter->IgnoredNodes.Add(NodeToCheck);

				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Removed node %s from %s"), *FBAUtils::GetNodeName(NodeToCheck), *FBAUtils::GetNodeName(ParentFormatter->GetRootNode()));
			}

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Added node %s to %s"), *FBAUtils::GetNodeName(NodeToCheck), *FBAUtils::GetNodeName(ParameterFormatter->GetRootNode()));
			ParameterParentMap.Add(NodeToCheck, ParameterFormatter);
		}

		// the next main nodes will ignore the input nodes from the parameter formatter
		IgnoredNodes.Append(ParameterFormatter->FormattedInputNodes.Array());
	}

	// Format once again with proper ignored nodes
	for (UEdGraphNode* MainNode : NodePoolCopy)
	{
		TSharedPtr<FEdGraphParameterFormatter> ParameterFormatter = GetParameterFormatter(MainNode);
		ParameterFormatter->FormatNode(MainNode);
	}

	// Expand parameters by height
	if (GetDefault<UBASettings>()->bExpandParametersByHeight)
	{
		for (UEdGraphNode* MainNode : NodePoolCopy)
		{
			TSharedPtr<FEdGraphParameterFormatter> ParameterFormatter = GetParameterFormatter(MainNode);
			ParameterFormatter->ExpandByHeight();
		}
	}

	// Save relative position
	for (auto& Elem : ParameterFormatterMap)
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("MainParamForm: %s"), *FBAUtils::GetNodeName(Elem.Key));

		TSharedPtr<FEdGraphParameterFormatter> ParamFormatter = Elem.Value;
		ParamFormatter->SaveRelativePositions();
		ParamFormatter->bInitialized = true;

		// for (auto Child : ParamFormatter->GetFormattedNodes())
		// {
		// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\tNode %s"), *FBAUtils::GetNodeName(Child));
		// }
	}
}

TSet<UEdGraphNode*> FEdGraphFormatter::GetFormattedGraphNodes()
{
	TSet<UEdGraphNode*> OutNodes;
	for (UEdGraphNode* Node : NodePool)
	{
		OutNodes.Append(GetParameterFormatter(Node)->GetFormattedNodes());
	}

	return OutNodes;
}

void FEdGraphFormatter::RefreshParameters(UEdGraphNode* Node)
{
	if (FBAUtils::IsNodePure(Node))
	{
		return;
	}

	TSharedPtr<FEdGraphParameterFormatter> Formatter = GetParameterFormatter(Node);
	Formatter->FormatNode(Node);
}

bool FEdGraphFormatter::IsFormattingRequired(const TArray<UEdGraphNode*>& NewNodeTree)
{
	if (!NewNodeTree.Contains(NodeToKeepStill))
	{
		return true;
	}

	// Check if a node has been deleted
	if (NodeTree.ContainsByPredicate(FBAUtils::IsNodeDeleted))
	{
		//UE_LOG(LogBlueprintAssist, Warning, TEXT("One of the nodes has been deleted"));
		return true;
	}

	// Check if the number of nodes has changed
	if (NodeTree.Num() != NewNodeTree.Num())
	{
		//UE_LOG(LogBlueprintAssist, Warning, TEXT("Num nodes changed %d to %d"), NewNodeTree.Num(), NodeTree.Num());
		return true;
	}

	// Check if the node tree has changed
	for (UEdGraphNode* Node : NewNodeTree)
	{
		if (!NodeTree.Contains(Node))
		{
			//UE_LOG(LogBlueprintAssist, Warning, TEXT("Node tree changed for node %s"), *FBAUtils::GetNodeName(Node));
			return true;
		}
	}

	// Check if any formatted nodes from last time have changed position or links
	for (UEdGraphNode* Node : GetFormattedNodes())
	{
		// Check if node has changed
		if (NodeChangeInfos.Contains(Node))
		{
			FNodeChangeInfo ChangeInfo = NodeChangeInfos[Node];
			if (ChangeInfo.HasChanged(NodeToKeepStill))
			{
				//UE_LOG(LogBlueprintAssist, Warning, TEXT("Node links or position has changed"));
				return true;
			}
		}
	}

	TArray<UEdGraphNode_Comment*> CachedComments;
	CommentNodesContains.GetKeys(CachedComments);

	// Check if any comment nodes have been deleted
	const auto& NodeTreeCapture = NodeTree;
	TArray<UEdGraphNode_Comment*> CommentNodes = FBAUtils::GetCommentNodesFromGraph(GraphHandler->GetFocusedEdGraph());
	for (auto Comment : CommentNodes)
	{
		TArray<UEdGraphNode*> Contained = FBAUtils::GetNodesUnderComment(Comment);
		UEdGraphNode** RelativeNodePtr = Contained.FindByPredicate([&NodeTreeCapture](UEdGraphNode* Node)
		{
			return NodeTreeCapture.Contains(Node);
		});

		if (RelativeNodePtr != nullptr)
		{
			if (!CachedComments.Contains(Comment))
			{
				return true;
			}
		}
	}

	// Check contained comment nodes
	for (UEdGraphNode_Comment* Comment : CachedComments)
	{
		if (!CommentNodes.Contains(Comment))
		{
			return true;
		}

		TArray<UEdGraphNode*>& CachedContained = CommentNodesContains[Comment];
		TArray<UEdGraphNode*> CurrentContained = FBAUtils::GetNodesUnderComment(Comment);
		if (CachedContained.Num() != CurrentContained.Num())
		{
			return true;
		}

		for (auto Contained : CachedContained)
		{
			if (!CurrentContained.Contains(Contained))
			{
				return true;
			}
		}
	}

	return false;
}

void FEdGraphFormatter::SaveFormattingEndInfo()
{
	// Save the position so we can move relative to this the next time we format
	LastFormattedX = NodeToKeepStill->NodePosX;
	LastFormattedY = NodeToKeepStill->NodePosY;

	// Save node information
	for (UEdGraphNode* Node : GetFormattedNodes())
	{
		if (NodeChangeInfos.Contains(Node))
		{
			NodeChangeInfos[Node].UpdateValues(NodeToKeepStill);
		}
		else
		{
			NodeChangeInfos.Add(Node, FNodeChangeInfo(Node, NodeToKeepStill));
		}
	}
}

TArray<UEdGraphNode*> FEdGraphFormatter::GetNodeTree(UEdGraphNode* InitialNode) const
{
	const auto& GraphHandlerCapture = GraphHandler;
	const auto& FormatterParametersCapture = FormatterParameters;
	const auto Filter = [&GraphHandlerCapture, &FormatterParametersCapture](const FPinLink& Link)
	{
		return GraphHandlerCapture->FilterDelegatePin(Link, FormatterParametersCapture.NodesToFormat);
	};
	return FBAUtils::GetNodeTreeWithFilter(InitialNode, Filter).Array();
}

bool FEdGraphFormatter::IsInitialNodeValid(UEdGraphNode* Node) const
{
	if (!Node)
	{
		return false;
	}
	if (Cast<UEdGraphNode_Comment>(Node))
	{
		return false;
	}
	if (Cast<UK2Node_Knot>(Node))
	{
		return false;
	}

	return true;
}

FSlateRect FEdGraphFormatter::GetClusterBounds(UEdGraphNode* Node)
{
	const TArray<UEdGraphNode*> Nodes = GetParameterFormatter(Node)->GetFormattedNodes().Array();
	return FBAUtils::GetCachedNodeArrayBounds(GraphHandler, Nodes);
}

FSlateRect FEdGraphFormatter::GetRelativeClusterBounds(UEdGraphNode* Node, UEdGraphNode* NodeAsking)
{
	const TArray<UEdGraphNode*> ParameterNodes = GetParameterFormatter(Node)->GetFormattedNodes().Array();

	if (ParameterNodes.Num() == 0)
	{
		return FBAUtils::GetCachedNodeBounds(GraphHandler, Node);
	}

	bool bBoundsInit = false;
	FSlateRect Bounds;
	for (int i = 0; i < ParameterNodes.Num(); i++)
	{
		UEdGraphNode* ParameterNode = ParameterNodes[i];
		if (!ParameterNode)
		{
			continue;
		}

		// initialize bounds from first valid node
		if (!bBoundsInit)
		{
			Bounds = GetRelativeNodeBounds(ParameterNode, NodeAsking, true);
			bBoundsInit = true;
		}
		else
		{
			Bounds = Bounds.Expand(GetRelativeNodeBounds(ParameterNode, NodeAsking, true));
		}
	}

	return Bounds;
}

FSlateRect FEdGraphFormatter::GetRelativeBoundsForNodes(const TArray<UEdGraphNode*>& Nodes, UEdGraphNode* NodeAsking, bool bUseClusterBounds)
{
	TArray<FSlateRect> AllBounds;

	for (UEdGraphNode* Node : Nodes)
	{
		AllBounds.Add(GetRelativeNodeBounds(Node, NodeAsking, bUseClusterBounds));
	}

	return FBAUtils::GetGroupedBounds(AllBounds);
}

FSlateRect FEdGraphFormatter::GetRelativeBoundsForNodes(const TArray<UEdGraphNode*>& Nodes, UEdGraphNode* NodeAsking, TSet<UEdGraphNode*>& RelativeNodes, bool bUseClusterBounds)
{
	TArray<FSlateRect> AllBounds;

	for (UEdGraphNode* Node : Nodes)
	{
		AllBounds.Add(GetRelativeNodeBounds(Node, NodeAsking, RelativeNodes, bUseClusterBounds));
	}

	return FBAUtils::GetGroupedBounds(AllBounds);
}

FSlateRect FEdGraphFormatter::GetClusterBoundsForNodes(const TArray<UEdGraphNode*>& Nodes)
{
	TArray<UEdGraphNode*> NodesInColumn;

	for (UEdGraphNode* Node : Nodes)
	{
		if (Node)
		{
			NodesInColumn.Append(GetParameterFormatter(Node)->GetFormattedNodes().Array());
		}
	}

	return FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesInColumn);
}

TSharedPtr<FEdGraphParameterFormatter> FEdGraphFormatter::GetParameterFormatter(UEdGraphNode* Node)
{
	if (!ParameterFormatterMap.Contains(Node))
	{
		ParameterFormatterMap.Add(Node, MakeShared<FEdGraphParameterFormatter>(GraphHandler, Node, SharedThis(this)));
	}

	return ParameterFormatterMap[Node];
}

TSet<UEdGraphNode*> FEdGraphFormatter::GetFormattedNodes()
{
	if (MainParameterFormatter.IsValid())
	{
		return MainParameterFormatter->GetFormattedNodes();
	}

	TSet<UEdGraphNode*> OutNodes;
	for (UEdGraphNode* Node : NodePool)
	{
		OutNodes.Append(GetParameterFormatter(Node)->GetFormattedNodes());
	}

	OutNodes.Append(KnotNodesSet);

	return OutNodes;
}

void FEdGraphFormatter::FormatY()
{
	NodeHeightLevels.Add(RootNode, 0);

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("-------Format Y-------- NO COMMENTS"));

	bAccountForComments = false;
	TSet<UEdGraphNode*> NodesToCollisionCheck;
	TSet<FPinLink> VisitedLinks;
	TSet<UEdGraphNode*> TempChildren;
	FormatY_Recursive(RootNode, nullptr, nullptr, NodesToCollisionCheck, VisitedLinks, true, TempChildren);

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("-------Format Y-------- COMMENTS"));

	bAccountForComments = GetDefault<UBASettings>()->bAccountForCommentsWhenFormatting;
	NodesToCollisionCheck.Reset();
	VisitedLinks.Reset();
	TempChildren.Reset();
	FormatY_Recursive(RootNode, nullptr, nullptr, NodesToCollisionCheck, VisitedLinks, true, TempChildren);
}

void FEdGraphFormatter::CenterBranches(UEdGraphNode* CurrentNode, TArray<ChildBranch>& ChildBranches, TSet<UEdGraphNode*>& NodesToCollisionCheck)
{
	// Center branches
	TArray<UEdGraphPin*> ChildPins;
	TArray<UEdGraphPin*> ParentPins;
	for (ChildBranch& Branch : ChildBranches)
	{
		ChildPins.Add(Branch.Pin);
		ParentPins.Add(Branch.ParentPin);
	}

	const float ChildrenCenter = FBAUtils::GetCenterYOfPins(GraphHandler, ChildPins);
	const float ParentCenter = FBAUtils::GetCenterYOfPins(GraphHandler, ParentPins);
	const float Offset = ParentCenter - ChildrenCenter;

	TArray<UEdGraphNode*> AllNodes;

	for (ChildBranch& Branch : ChildBranches)
	{
		for (UEdGraphNode* Child : Branch.BranchNodes)
		{
			AllNodes.Add(Child);
			Child->NodePosY += Offset;
			RefreshParameters(Child);
		}
	}

	// Resolve collisions
	AllNodes.Add(CurrentNode);
	FSlateRect AllNodesBounds = bAccountForComments ? GetRelativeBoundsForNodes(AllNodes, CurrentNode, true) : GetClusterBoundsForNodes(AllNodes);
	const float InitialTop = AllNodesBounds.Top;
	for (auto Node : NodesToCollisionCheck)
	{
		if (AllNodes.Contains(Node))
		{
			continue;
		}

		FSlateRect Bounds = bAccountForComments ? GetRelativeNodeBounds(Node, CurrentNode, true) : GetClusterBounds(Node);
		Bounds = Bounds.ExtendBy(FMargin(0, 0, 0, NodePadding.Y));
		if (FSlateRect::DoRectanglesIntersect(Bounds, AllNodesBounds))
		{
			const float OffsetY = Bounds.Bottom - AllNodesBounds.Top;
			AllNodesBounds = AllNodesBounds.OffsetBy(FVector2D(0, OffsetY));
		}
	}

	const float DeltaY = AllNodesBounds.Top - InitialTop;
	if (DeltaY != 0)
	{
		for (auto Node : AllNodes)
		{
			Node->NodePosY += DeltaY;
			RefreshParameters(Node);
		}
	}
}

bool FEdGraphFormatter::TryAlignTrackToEndPins(TSharedPtr<FKnotNodeTrack> Track, const TArray<UEdGraphNode*>& AllNodes)
{
	const float ParentPinY = GraphHandler->GetPinY(Track->ParentPin);
	const float LastPinY = GraphHandler->GetPinY(Track->GetLastPin());
	bool bPreferParentPin = ParentPinY > LastPinY;

	if (FBAUtils::IsExecPin(Track->ParentPin))
	{
		bPreferParentPin = true;
	}

	for (int i = 0; i < 2; ++i)
	{
		//FString PreferPinStr = bPreferParentPin ? "true" : "false";
		//UE_LOG(LogBlueprintAssist, Warning, TEXT("AlignTrack ParentY %f (%s) | LastPinY %f (%s) | PreferParent %s"), ParentPinY, *FBAUtils::GetPinName(Track->ParentPin), LastPinY,
		//       *FBAUtils::GetPinName(Track->GetLastPin()), *PreferPinStr);

		if (i == 1)
		{
			bPreferParentPin = !bPreferParentPin;
		}

		UEdGraphPin* SourcePin = bPreferParentPin ? Track->ParentPin : Track->GetLastPin();
		UEdGraphPin* OtherPin = bPreferParentPin ? Track->GetLastPin() : Track->ParentPin;

		const FVector2D SourcePinPos = FBAUtils::GetPinPos(GraphHandler, SourcePin);
		const FVector2D OtherPinPos = FBAUtils::GetPinPos(GraphHandler, OtherPin);

		const FVector2D Padding = FBAUtils::IsParameterPin(OtherPin)
			? PinPadding
			: NodePadding;

		const FVector2D Point
			= SourcePin->Direction == EGPD_Output
			? FVector2D(OtherPinPos.X - Padding.X, SourcePinPos.Y)
			: FVector2D(OtherPinPos.X + Padding.X, SourcePinPos.Y);

		// UE_LOG(LogBlueprintAssist, Error, TEXT("Checking Point %s | %s"), *Point.ToString(), *FBAUtils::GetNodeName(SourcePin->GetOwningNode()));

		bool bAnyCollision = false;

		for (UEdGraphNode* NodeToCollisionCheck : AllNodes)
		{
			FSlateRect CollisionBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, NodeToCollisionCheck).ExtendBy(FMargin(0, TrackSpacing - 1));

			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Collision check against %s | %s | %s"), *FBAUtils::GetNodeName(NodeToCollisionCheck), *CollisionBounds.ToString(), *Point.ToString());

			if (NodeToCollisionCheck == SourcePin->GetOwningNode() || NodeToCollisionCheck == OtherPin->GetOwningNode())
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tSkipping node"));
				continue;
			}

			if (FBAUtils::LineRectIntersection(CollisionBounds, SourcePinPos, Point))
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tFound collision"));
				bAnyCollision = true;
				break;
			}
		}

		for (TSharedPtr<FKnotNodeTrack> OtherTrack : KnotTracks)
		{
			if (OtherTrack == Track)
			{
				continue;
			}

			// Possibly revert back to rect collision check
			if (FBAUtils::LineRectIntersection(OtherTrack->GetTrackBounds().ExtendBy(FMargin(0, TrackSpacing * 0.25f)), SourcePinPos, Point))
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Track %s colliding with %s"), *Track->ToString(), *OtherTrack->ToString());
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tStart %s End %s"), *SourcePinPos.ToString(), *Point.ToString());
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tRect %s"), *MyTrackBounds.ToString());
				bAnyCollision = true;
				break;
			}
		}

		if (!bAnyCollision)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("sucessfully found easy solution!"));
			Track->PinAlignedX = Point.X;
			Track->UpdateTrackHeight(SourcePinPos.Y);
			Track->PinToAlignTo = SourcePin;
			return true;
		}
	}

	return false;
}

bool FEdGraphFormatter::DoesPinNeedTrack(UEdGraphPin* Pin, const TArray<UEdGraphPin*>& LinkedTo)
{
	if (LinkedTo.Num() == 0)
	{
		return false;
	}

	// if the pin is linked to multiple linked nodes, we need a knot track
	if (LinkedTo.Num() > 1)
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Multiple linked to?"));
		return true;
	}

	// otherwise the pin is linked to exactly 1 node, run a collision check
	UEdGraphPin* OtherPin = LinkedTo[0];

	// need pin if there are any collisions
	return AnyCollisionBetweenPins(Pin, OtherPin);
}

bool FEdGraphFormatter::AnyCollisionBetweenPins(UEdGraphPin* Pin, UEdGraphPin* OtherPin)
{
	TSet<UEdGraphNode*> FormattedNodes = GetFormattedGraphNodes();

	const FVector2D PinPos = FBAUtils::GetPinPos(GraphHandler, Pin);
	const FVector2D OtherPinPos = FBAUtils::GetPinPos(GraphHandler, OtherPin);

	return NodeCollisionBetweenLocation(PinPos, OtherPinPos, { Pin->GetOwningNode(), OtherPin->GetOwningNode() });
}

bool FEdGraphFormatter::NodeCollisionBetweenLocation(FVector2D Start, FVector2D End, TSet<UEdGraphNode*> IgnoredNodes)
{
	TSet<UEdGraphNode*> FormattedNodes = GetFormattedGraphNodes();

	for (UEdGraphNode* NodeToCollisionCheck : FormattedNodes)
	{
		if (IgnoredNodes.Contains(NodeToCollisionCheck))
		{
			continue;
		}

		FSlateRect NodeBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, NodeToCollisionCheck).ExtendBy(FMargin(0, TrackSpacing - 1));
		if (FBAUtils::LineRectIntersection(NodeBounds, Start, End))
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tNode collision!"));
			return true;
		}
	}

	return false;
}

void FEdGraphFormatter::MakeKnotTrack()
{
	const TSet<UEdGraphNode*> FormattedNodes = GetFormattedGraphNodes();

	auto& GraphHandlerCapture = GraphHandler;
	auto FormatterParametersCapture = FormatterParameters;
	const auto& NotFormatted = [FormattedNodes, GraphHandlerCapture, FormatterParametersCapture](UEdGraphPin* Pin)
	{
		return !FormattedNodes.Contains(Pin->GetOwningNode()) || !GraphHandlerCapture->FilterSelectiveFormatting(Pin->GetOwningNode(), FormatterParametersCapture.NodesToFormat);
	};

	// iterate across the pins of all nodes and determine if they require a knot track
	for (UEdGraphNode* MyNode : FormattedNodes)
	{
		// make tracks for input exec pins
		TArray<TSharedPtr<FKnotNodeTrack>> PreviousTracks;
		for (UEdGraphPin* MyPin : FBAUtils::GetExecPins(MyNode, EGPD_Input))
		{
			TArray<UEdGraphPin*> LinkedTo = MyPin->LinkedTo;
			LinkedTo.RemoveAll(NotFormatted);
			if (LinkedTo.Num() == 0)
			{
				continue;
			}

			if (GetMutableDefault<UBASettings>()->ExecutionWiringStyle == EBAWiringStyle::AlwaysMerge)
			{
				MakeKnotTracksForLinkedExecPins(MyPin, LinkedTo, PreviousTracks);
			}
			else
			{
				for (UEdGraphPin* Pin : LinkedTo)
				{
					MakeKnotTracksForLinkedExecPins(MyPin, { Pin }, PreviousTracks);
				}
			}
		}
	}

	for (UEdGraphNode* MyNode : FormattedNodes)
	{
		// make tracks for output parameter pins
		TArray<TSharedPtr<FKnotNodeTrack>> PreviousTracks;
		for (UEdGraphPin* MyPin : FBAUtils::GetParameterPins(MyNode, EGPD_Output))
		{
			TArray<UEdGraphPin*> LinkedTo = MyPin->LinkedTo;
			LinkedTo.RemoveAll(NotFormatted);
			if (LinkedTo.Num() == 0)
			{
				continue;
			}

			if (GetMutableDefault<UBASettings>()->ParameterWiringStyle == EBAWiringStyle::AlwaysMerge)
			{
				MakeKnotTracksForParameterPins(MyPin, LinkedTo, PreviousTracks);
			}
			else
			{
				for (UEdGraphPin* Pin : LinkedTo)
				{
					MakeKnotTracksForParameterPins(MyPin, { Pin }, PreviousTracks);
				}
			}
		}
	}
}

TSharedPtr<FKnotNodeTrack> FEdGraphFormatter::MakeKnotTracksForLinkedExecPins(UEdGraphPin* ParentPin, TArray<UEdGraphPin*> LinkedPins, TArray<TSharedPtr<FKnotNodeTrack>>& PreviousTracks)
{
	FVector2D ParentPinPos = FBAUtils::GetPinPos(GraphHandler, ParentPin);
	UEdGraphNode* ParentNode = ParentPin->GetOwningNode();

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Processing knot track for parent pin %s"), *FBAUtils::GetPinName(ParentPin));
	// for (auto Pin : LinkedPins)
	// {
	// 	UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *FBAUtils::GetPinName(Pin));
	// }

	// check for looping pins, these are pins where 
	// the x position of the pin is less than the x value of the parent pin
	TArray<UEdGraphPin*> LoopingPins;
	for (UEdGraphPin* LinkedPin : LinkedPins)
	{
		const FVector2D LinkedPinPos = FBAUtils::GetPinPos(GraphHandler, LinkedPin);
		if (LinkedPinPos.X > ParentPinPos.X)
		{
			LoopingPins.Add(LinkedPin);
		}
	}

	// create looping tracks
	for (UEdGraphPin* OtherPin : LoopingPins)
	{
		const float OtherNodeTop = FBAUtils::GetNodeBounds(OtherPin->GetOwningNode()).Top;
		const float MyNodeTop = FBAUtils::GetNodeBounds(ParentNode).Top;
		const float AboveNodeWithPadding = FMath::Min(OtherNodeTop, MyNodeTop) - TrackSpacing * 2;

		TArray<UEdGraphPin*> TrackPins = { OtherPin };
		TSharedPtr<FKnotNodeTrack> KnotTrack = MakeShared<FKnotNodeTrack>(SharedThis(this), GraphHandler, ParentPin, TrackPins, AboveNodeWithPadding, true);
		KnotTracks.Add(KnotTrack);

		const FVector2D OtherPinPos = FBAUtils::GetPinPos(GraphHandler, OtherPin);

		const FVector2D FirstKnotPos(ParentPinPos.X + 20, KnotTrack->GetTrackHeight());
		TSharedPtr<FKnotNodeCreation> FirstLoopingKnot = MakeShared<FKnotNodeCreation>(KnotTrack, FirstKnotPos, nullptr, OtherPin);
		KnotTrack->KnotCreations.Add(FirstLoopingKnot);

		const FVector2D SecondKnotPos(OtherPinPos.X - 20, KnotTrack->GetTrackHeight());
		TSharedPtr<FKnotNodeCreation> SecondLoopingKnot = MakeShared<FKnotNodeCreation>(KnotTrack, SecondKnotPos, FirstLoopingKnot, OtherPin);
		KnotTrack->KnotCreations.Add(SecondLoopingKnot);
	}

	LinkedPins.RemoveAll([&LoopingPins](UEdGraphPin* Pin) { return LoopingPins.Contains(Pin); });

	// remove pins which are left or too close to my pin
	const float Threshold = ParentPinPos.X - NodePadding.X * 1.5f;
	TSharedPtr<FBAGraphHandler> GraphHandlerRef = GraphHandler;
	const auto& IsTooCloseToParent = [GraphHandlerRef, Threshold](UEdGraphPin* Pin)
	{
		const FVector2D PinPos = FBAUtils::GetPinPos(GraphHandlerRef, Pin);
		return PinPos.X > Threshold;
	};
	LinkedPins.RemoveAll(IsTooCloseToParent);

	// remove any linked pins which has the same height and no collision
	UEdGraphPin* PinRemoved = nullptr;
	for (UEdGraphPin* LinkedPin : LinkedPins)
	{
		//UE_LOG(LogBlueprintAssist, Warning, TEXT("Checking %s"), *FBAUtils::GetPinName(LinkedPin));
		const FVector2D LinkedPinPos = FBAUtils::GetPinPos(GraphHandler, LinkedPin);
		const bool bSameHeight = FMath::Abs(LinkedPinPos.Y - ParentPinPos.Y) < 5.f;
		if (bSameHeight && !AnyCollisionBetweenPins(ParentPin, LinkedPin))
		{
			PinRemoved = LinkedPin;
			break;
		}
	}
	LinkedPins.Remove(PinRemoved);

	if (LinkedPins.Num() == 0)
	{
		return nullptr;
	}

	// sort pins by node's highest x position first then highest y position
	const auto RightTop = [](const UEdGraphPin& PinA, const UEdGraphPin& PinB)
	{
		if (PinA.GetOwningNode()->NodePosX == PinB.GetOwningNode()->NodePosX)
		{
			return PinA.GetOwningNode()->NodePosY > PinB.GetOwningNode()->NodePosY;
		}

		return PinA.GetOwningNode()->NodePosX > PinB.GetOwningNode()->NodePosX;
	};

	LinkedPins.Sort(RightTop);

	const FVector2D LastPinPos = FBAUtils::GetPinPos(GraphHandler, LinkedPins.Last());

	const float Dist = FMath::Abs(ParentPinPos.X - LastPinPos.X);

	// skip the pin distance check if we are expanding by height
	const bool bPinReallyFar = Dist > GetDefault<UBASettings>()->KnotNodeDistanceThreshold && !GetDefault<UBASettings>()->bExpandNodesByHeight;

	const bool bPinNeedsTrack = DoesPinNeedTrack(ParentPin, LinkedPins);

	const bool bPreviousHasTrack = PreviousTracks.Num() > 0;

	const FVector2D ToLast = LastPinPos - ParentPinPos;
	const bool bTooSteep = FMath::Abs(ToLast.Y) / FMath::Abs(ToLast.X) >= 2.75f;
	if (bTooSteep)
	{
		return nullptr;
	}

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Need reroute: %d %d %d"), bPinReallyFar, bPreviousHasTrack, bPinNeedsTrack);

	const bool bNeedsReroute = bPinReallyFar || bPreviousHasTrack || bPinNeedsTrack;
	if (!bNeedsReroute)
	{
		return nullptr;
	}

	TSharedPtr<FKnotNodeTrack> KnotTrack = MakeShared<FKnotNodeTrack>(SharedThis(this), GraphHandler, ParentPin, LinkedPins, ParentPinPos.Y, false);
	KnotTracks.Add(KnotTrack);

	TryAlignTrackToEndPins(KnotTrack, GetFormattedGraphNodes().Array());

	// if the track is not at the same height as the pin, then we need an
	// initial knot right of the inital pin, at the track height
	const FVector2D MyKnotPos = FVector2D(ParentPinPos.X - NodePadding.X, KnotTrack->GetTrackHeight());
	TSharedPtr<FKnotNodeCreation> PreviousKnot = MakeShared<FKnotNodeCreation>(KnotTrack, MyKnotPos, nullptr, KnotTrack->ParentPin);
	KnotTrack->KnotCreations.Add(PreviousKnot);

	// create a knot node for each of the pins remaining in linked to
	for (UEdGraphPin* OtherPin : KnotTrack->LinkedTo)
	{
		ParentPin->BreakLinkTo(OtherPin);

		const FVector2D OtherPinPos = FBAUtils::GetPinPos(GraphHandler, OtherPin);
		const float KnotX = FMath::Min(OtherPinPos.X + NodePadding.X, ParentPinPos.X - NodePadding.X);
		const FVector2D KnotPos(KnotX, KnotTrack->GetTrackHeight());

		// if the x position is very close to the previous knot's x position, 
		// we should not need to create a new knot instead we merge the locations
		if (PreviousKnot.IsValid() && FMath::Abs(KnotX - PreviousKnot->KnotPos.X) < 50)
		{
			PreviousKnot->KnotPos.X = KnotX;
			PreviousKnot->PinHandlesToConnectTo.Add(OtherPin);
			continue;
		}

		PreviousKnot = MakeShared<FKnotNodeCreation>(KnotTrack, KnotPos, PreviousKnot, OtherPin);
		KnotTrack->KnotCreations.Add(PreviousKnot);
	}

	PreviousTracks.Add(KnotTrack);

	return KnotTrack;
}

TSharedPtr<FKnotNodeTrack> FEdGraphFormatter::MakeKnotTracksForParameterPins(UEdGraphPin* ParentPin, TArray<UEdGraphPin*> LinkedPins, TArray<TSharedPtr<FKnotNodeTrack>>& PreviousTracks)
{
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Make knot tracks for parameter pin %s"), *FBAUtils::GetPinName(ParentPin));

	FVector2D ParentPinPos = FBAUtils::GetPinPos(GraphHandler, ParentPin);

	// remove pins which are left or too close to my pin
	const float Threshold = ParentPinPos.X + NodePadding.X * 2.0f;
	TSharedPtr<FBAGraphHandler> GraphHandlerRef = GraphHandler;

	const auto& IsTooCloseToParent = [GraphHandlerRef, Threshold](UEdGraphPin* Pin)
	{
		const FVector2D PinPos = FBAUtils::GetPinPos(GraphHandlerRef, Pin);
		return PinPos.X < Threshold;
	};

	LinkedPins.RemoveAll(IsTooCloseToParent);

	if (LinkedPins.Num() == 0)
	{
		return nullptr;
	}

	const auto LeftTop = [](const UEdGraphPin& PinA, const UEdGraphPin& PinB)
	{
		if (PinA.GetOwningNode()->NodePosX == PinB.GetOwningNode()->NodePosX)
		{
			return PinA.GetOwningNode()->NodePosY > PinB.GetOwningNode()->NodePosY;
		}

		return PinA.GetOwningNode()->NodePosX < PinB.GetOwningNode()->NodePosX;
	};
	LinkedPins.Sort(LeftTop);

	const FVector2D LastPinPos = FBAUtils::GetPinPos(GraphHandler, LinkedPins.Last());

	const float Dist = FMath::Abs(ParentPinPos.X - LastPinPos.X);

	const bool bLastPinFarAway = Dist > GetMutableDefault<UBASettings>()->KnotNodeDistanceThreshold && !GetDefault<UBASettings>()->bExpandNodesByHeight;

	const bool bPinNeedsTrack = DoesPinNeedTrack(ParentPin, LinkedPins);

	const bool bPreviousHasTrack = PreviousTracks.Num() > 0;

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Needs track: %d %d %d"), bLastPinFarAway, bPreviousHasTrack, bPinNeedsTrack);

	const FVector2D ToLast = LastPinPos - ParentPinPos;
	const bool bTooSteep = FMath::Abs(ToLast.Y) / FMath::Abs(ToLast.X) >= 2.75f;
	if (bTooSteep)
	{
		return nullptr;
	}

	const bool bNeedsReroute = bPinNeedsTrack || bPreviousHasTrack || bLastPinFarAway;
	if (!bNeedsReroute)
	{
		return nullptr;
	}

	// init the knot track
	TSharedPtr<FKnotNodeTrack> KnotTrack = MakeShared<FKnotNodeTrack>(SharedThis(this), GraphHandler, ParentPin, LinkedPins, ParentPinPos.Y, false);
	KnotTracks.Add(KnotTrack);

	// check if the track height can simply be set to one of it's pin's height
	if (TryAlignTrackToEndPins(KnotTrack, GetFormattedGraphNodes().Array()))
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Found a pin to align to for %s"), *FBAUtils::GetPinName(KnotTrack->ParentPin));
	}
	else
	{
		// UE_LOG(LogBlueprintAssist, Warning, TEXT("Failed to find pin to align to"));
	}

	// Add a knot creation which links to the parent pin
	const FVector2D InitialKnotPos = FVector2D(ParentPinPos.X + PinPadding.X, KnotTrack->GetTrackHeight());
	TSharedPtr<FKnotNodeCreation> PreviousKnot = MakeShared<FKnotNodeCreation>(KnotTrack, InitialKnotPos, nullptr, KnotTrack->ParentPin);
	ParentPin->BreakLinkTo(KnotTrack->GetLastPin());
	KnotTrack->KnotCreations.Add(PreviousKnot);

	for (UEdGraphPin* OtherPin : KnotTrack->LinkedTo)
	{
		// break link to parent pin
		ParentPin->BreakLinkTo(OtherPin);

		const FVector2D OtherPinPos = FBAUtils::GetPinPos(GraphHandler, OtherPin);
		const float KnotX = FMath::Max(OtherPinPos.X - PinPadding.X, ParentPinPos.X + PinPadding.X);

		const FVector2D KnotPos = FVector2D(KnotX, KnotTrack->GetTrackHeight());

		// if the x position is very close to the previous knot's x position, 
		// we should not need to create a new knot instead we merge the locations
		if (PreviousKnot.IsValid() && FMath::Abs(KnotX - PreviousKnot->KnotPos.X) < 50)
		{
			PreviousKnot->KnotPos.X = KnotX;
			PreviousKnot->PinHandlesToConnectTo.Add(OtherPin);
			continue;
		}

		// Add a knot creation for each linked pin
		PreviousKnot = MakeShared<FKnotNodeCreation>(KnotTrack, KnotPos, PreviousKnot, OtherPin);
		KnotTrack->KnotCreations.Add(PreviousKnot);
	}

	PreviousTracks.Add(KnotTrack);

	return KnotTrack;
}

void FEdGraphFormatter::MergeNearbyKnotTracks()
{
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Merging knot track"));

	TArray<TSharedPtr<FKnotNodeTrack>> PendingTracks = KnotTracks;

	if (GetMutableDefault<UBASettings>()->ExecutionWiringStyle != EBAWiringStyle::MergeWhenNear)
	{
		PendingTracks.RemoveAll([](TSharedPtr<FKnotNodeTrack> Track)
		{
			return FBAUtils::IsExecPin(Track->ParentPin);
		});
	}

	if (GetMutableDefault<UBASettings>()->ParameterWiringStyle != EBAWiringStyle::MergeWhenNear)
	{
		PendingTracks.RemoveAll([](TSharedPtr<FKnotNodeTrack> Track)
		{
			return FBAUtils::IsParameterPin(Track->ParentPin);
		});
	}

	// TODO: Handle merging of looping tracks
	PendingTracks.RemoveAll([](TSharedPtr<FKnotNodeTrack> Track)
	{
		return Track->bIsLoopingTrack;
	});

	while (PendingTracks.Num() > 0)
	{
		auto CurrentTrack = PendingTracks.Pop();
		auto Tracks = PendingTracks;

		for (TSharedPtr<FKnotNodeTrack> Track : Tracks)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Merging track %s"), *FBAUtils::GetPinName(Track->ParentPin));

			// merge if they have the same parent and same height
			if (Track->ParentPin == CurrentTrack->ParentPin &&
				Track->GetTrackHeight() == CurrentTrack->GetTrackHeight())
			{
				for (TSharedPtr<FKnotNodeCreation> Creation : Track->KnotCreations)
				{
					bool bShouldAddCreation = true;
					for (TSharedPtr<FKnotNodeCreation> CurrentCreation : CurrentTrack->KnotCreations)
					{
						if (FMath::Abs(CurrentCreation->KnotPos.X - Creation->KnotPos.X) < 50)
						{
							bShouldAddCreation = false;
							CurrentCreation->PinHandlesToConnectTo.Append(Creation->PinHandlesToConnectTo);
						}
					}

					if (bShouldAddCreation)
					{
						CurrentTrack->KnotCreations.Add(Creation);
						CurrentTrack->PinToAlignTo = nullptr;

						// UE_LOG(LogBlueprintAssist, Warning, TEXT("Cancelled pin to align to for track %s"), *FBAUtils::GetPinName(CurrentTrack->ParentPin));
					}
				}

				KnotTracks.Remove(Track);
				PendingTracks.Remove(Track);
			}
		}
	}
}
