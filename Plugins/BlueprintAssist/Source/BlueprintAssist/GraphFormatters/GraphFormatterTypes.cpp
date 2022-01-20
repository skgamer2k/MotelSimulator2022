// Copyright 2021 fpwong. All Rights Reserved.

#include "GraphFormatterTypes.h"

#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistUtils.h"
#include "EdGraphFormatter.h"
#include "EdGraphNode_Comment.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor/BlueprintGraph/Classes/K2Node_Knot.h"
#include "Runtime/SlateCore/Public/Layout/SlateRect.h"

FNodeInfo::FNodeInfo(
	UEdGraphNode* InNode,
	UEdGraphPin* InPin,
	TSharedPtr<FNodeInfo> InParent,
	UEdGraphPin* InParentPin,
	const EEdGraphPinDirection InDirection)
	: Node(InNode)
	, Pin(InPin)
	, Direction(InDirection)
	, Children(TArray<TSharedPtr<FNodeInfo>>()) {}

void FNodeInfo::SetParent(TSharedPtr<FNodeInfo> NewParent, UEdGraphPin* MyPin)
{
	Pin = MyPin;

	if (Parent.IsValid())
	{
		Parent->Children.Remove(SharedThis(this));
	}

	if (NewParent.IsValid() && NewParent != Parent)
	{
		NewParent->Children.Add(SharedThis(this));
	}

	Parent = NewParent;
}

int32 FNodeInfo::GetChildX(
	UEdGraphNode* Child,
	TSharedPtr<FBAGraphHandler> GraphHandler,
	const FVector2D& Padding,
	const EEdGraphPinDirection ChildDirection) const
{
	const FSlateRect MyBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, Node);

	const float ChildWidth = FBAUtils::GetCachedNodeBounds(GraphHandler, Child).GetSize().X;

	const float NewNodePos
		= ChildDirection == EGPD_Input
		? MyBounds.Left - ChildWidth - Padding.X - 1
		: MyBounds.Right + Padding.X + 1;

	return FMath::RoundToInt(NewNodePos);
}

bool FNodeInfo::DetectCycle(TSharedPtr<FNodeInfo> OtherInfo)
{
	TArray<TSharedPtr<FNodeInfo>> PendingInfos;
	PendingInfos.Add(OtherInfo);

	while (PendingInfos.Num() > 0)
	{
		TSharedPtr<FNodeInfo> NextInfo = PendingInfos.Pop();
		for (TSharedPtr<FNodeInfo> Child : NextInfo->Children)
		{
			if (Child == SharedThis(this))
			{
				return true;
			}

			PendingInfos.Add(Child);
		}
	}

	return false;
}

void FNodeInfo::MoveChildren(
	TSharedPtr<FNodeInfo> Info,
	TSharedPtr<FBAGraphHandler> GraphHandler,
	const FVector2D& Padding,
	TSet<UEdGraphNode*>& TempVisited) const
{
	for (TSharedPtr<FNodeInfo> Child : Info->Children)
	{
		if (TempVisited.Contains(Child->Node))
		{
			continue;
		}

		TempVisited.Add(Child->Node);

		Child->Node->NodePosX = Info->GetChildX(Child->Node, GraphHandler, Padding, Child->Direction);
		MoveChildren(Child, GraphHandler, Padding, TempVisited);
	}
}

FString FNodeInfo::ToString() const
{
	UEdGraphNode* ParentNode = Parent.IsValid() ? Parent->Node : nullptr;

	return FString::Printf(
		TEXT("NodeInfo <%s> | Par <%s>"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		ParentNode ? *FBAUtils::GetNodeName(ParentNode) : *FString("null")
	);
}

TArray<UEdGraphNode*> FNodeInfo::GetChildNodes()
{
	TArray<UEdGraphNode*> ChildNodes;
	for (TSharedPtr<FNodeInfo> Info : Children)
	{
		ChildNodes.Emplace(Info->Node);
	}

	return ChildNodes;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

UEdGraphPin* FPinLink::GetFromPin() const
{
	return FBAUtils::GetPinFromGraph(FromHandle, Graph);
}

UEdGraphPin* FPinLink::GetToPin() const
{
	return FBAUtils::GetPinFromGraph(ToHandle, Graph);
}

UEdGraphNode* FPinLink::GetFromNode() const
{
	if (UEdGraphPin* Pin = GetFromPin())
	{
		return Pin->GetOwningNodeUnchecked();
	}

	return nullptr;
}

UEdGraphNode* FPinLink::GetToNode() const
{
	if (UEdGraphPin* Pin = GetToPin())
	{
		return Pin->GetOwningNodeUnchecked();
	}

	return nullptr;
}

UEdGraphNode* FPinLink::GetNode() const
{
	return To == nullptr ? FallbackNode : To->GetOwningNodeUnchecked();
}

EEdGraphPinDirection FPinLink::GetDirection() const
{
	return From != nullptr ? From->Direction.GetValue() : EGPD_Output;
}

FString FPinLink::ToString() const
{
	UEdGraphNode* ParentNode = From == nullptr ? nullptr : From->GetOwningNodeUnchecked();
	return FString::Printf(
		TEXT("<%s> to <%s> | Pin <%s> to <%s> "),
		*FBAUtils::GetNodeName(ParentNode),
		*FBAUtils::GetNodeName(GetNode()),
		*FBAUtils::GetPinName(From),
		*FBAUtils::GetPinName(To)
	);
}

//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

UEdGraphPin* FKnotNodeCreation::GetPinToConnectTo() const
{
	return FBAUtils::GetPinFromGraph(PinToConnectToHandle, OwningKnotTrack->GraphHandler->GetFocusedEdGraph());
}

UK2Node_Knot* FKnotNodeCreation::CreateKnotNode(const FVector2D InKnotPos, UEdGraphPin* PreviousPin, UK2Node_Knot* KnotNodeToReuse, UEdGraph* Graph)
{
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("Create knot node for pin %s"), *FBAUtils::GetPinName(PreviousPin));

	CreatedKnot = nullptr;

	UEdGraphPin* MainPinToConnectTo = FBAUtils::GetPinFromGraph(PinToConnectToHandle, Graph);

	if (KnotNodeToReuse == nullptr)
	{
		CreatedKnot = FBAUtils::CreateKnotNode(Graph, InKnotPos, MainPinToConnectTo, PreviousPin);
	}
	else
	{
		CreatedKnot = KnotNodeToReuse;
		FBAUtils::LinkKnotNodeBetween(KnotNodeToReuse, InKnotPos, MainPinToConnectTo, PreviousPin);
	}

	for (FGraphPinHandle& PinHandle : PinHandlesToConnectTo)
	{
		UEdGraphPin* Pin = FBAUtils::GetPinFromGraph(PinHandle, Graph);

		// UE_LOG(LogBlueprintAssist, Warning, TEXT("\tLinking to pin %s"), *FBAUtils::GetPinName(Pin));

		if (Pin->Direction == EGPD_Input)
		{
			CreatedKnot->GetOutputPin()->MakeLinkTo(Pin);
		}
		else
		{
			CreatedKnot->GetInputPin()->MakeLinkTo(Pin);
		}
	}

	return CreatedKnot;
}

bool FKnotNodeCreation::HasHeightDifference() const
{
	if (KnotToConnectTo.IsValid())
	{
		return KnotToConnectTo->KnotPos.Y == KnotPos.Y;
	}

	return false;
}

float FKnotNodeTrack::GetTrackHeight() const
{
	if (UEdGraphPin* Pin = GetPinToAlignTo())
	{
		return GraphHandler->GetPinY(Pin);
	}

	return TrackHeight;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

FKnotNodeTrack::FKnotNodeTrack(
	TSharedPtr<FEdGraphFormatter> Formatter,
	TSharedPtr<FBAGraphHandler> InGraphHandler,
	UEdGraphPin* InParentPin,
	TArray<UEdGraphPin*> InLinkedTo,
	float InTrackY,
	bool bInIsLoopingTrack)
	: GraphHandler(InGraphHandler)
	, ParentPin(InParentPin)
	, LinkedTo(InLinkedTo)
	, TrackHeight(InTrackY)
	, PinToAlignTo(nullptr)
	, PinAlignedX(0)
	, bIsLoopingTrack(bInIsLoopingTrack)
{
	ParentPinPos = FBAUtils::GetPinPos(GraphHandler, InParentPin);

	SetTrackHeight(Formatter);
}

UEdGraphPin* FKnotNodeTrack::GetParentPin() const
{
	return ParentPin;
}

UEdGraphPin* FKnotNodeTrack::GetLastPin() const
{
	return LinkedTo.Last();
}

UEdGraphPin* FKnotNodeTrack::GetPinToAlignTo() const
{
	return FBAUtils::GetPinFromGraph(PinToAlignTo, GraphHandler->GetFocusedEdGraph());
}

FSlateRect FKnotNodeTrack::GetTrackBounds() const
{
	const float TrackSpacing = GetDefault<UBASettings>()->BlueprintKnotTrackSpacing;
	const float LocalTrackY = GetTrackHeight();
	const float LastPinX = FBAUtils::GetPinPos(GraphHandler, GetLastPin()).X;
	const float TrackXLeft = FMath::Min(ParentPinPos.X, LastPinX) + 5;
	const float TrackXRight = FMath::Max(ParentPinPos.X, LastPinX) - 5;

	return FSlateRect(FVector2D(TrackXLeft, LocalTrackY - (TrackSpacing - 1) * 0.5f),
					FVector2D(TrackXRight, LocalTrackY + (TrackSpacing - 1) * 0.5f));
}

TArray<FGraphPinHandle> FKnotNodeTrack::GetLinkedToSafe()
{
	TArray<FGraphPinHandle> OutHandles;
	OutHandles.Reserve(LinkedTo.Num());
	for (UEdGraphPin* Pin : LinkedTo)
	{
		OutHandles.Add(Pin);
	}

	return OutHandles;
}

void FKnotNodeTrack::SetTrackHeight(TSharedPtr<FEdGraphFormatter> Formatter)
{
	const float TrackSpacing = GetDefault<UBASettings>()->BlueprintKnotTrackSpacing;
	const TArray<UEdGraphNode*>& AllNodes = Formatter->GetFormattedNodes().Array();

	UEdGraphPin* LastPin = GetLastPin();

	const float TrackStart = GetTrackBounds().Left + 10;
	const float TrackEnd = GetTrackBounds().Right - 10;

	//UE_LOG(LogBlueprintAssist, Error, TEXT("FindTrackHeight for pin (%s) | Start %f | End %f"), *FBAUtils::GetPinName(ParentPin), TrackStart, TrackEnd);

	// use looping track height which was set when track is created
	if (bIsLoopingTrack)
	{
		return;
	}

	// Try align track to the parent pin or last pin
	for (UEdGraphPin* Pin : { ParentPin, LastPin })
	{
		const float PinHeight = Formatter->GraphHandler->GetPinY(Pin);
		if (TryAlignTrack(Formatter, TrackStart, TrackEnd, PinHeight))
		{
			TrackHeight = PinHeight;
			return;
		}
	}

	const float StartingPoint = GraphHandler->GetPinY(LastPin);

	float TestSolution = StartingPoint;

	for (int i = 0; i < 100; ++i)
	{
		bool bNoCollisionInDirection = true;

		FVector2D StartPoint(TrackStart, TestSolution);
		FVector2D EndPoint(TrackEnd, TestSolution);

		for (UEdGraphNode* NodeToCollisionCheck : AllNodes)
		{
			FSlateRect NodeBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, NodeToCollisionCheck).ExtendBy(FMargin(0, TrackSpacing - 1));

			const bool bSkipNode = NodeToCollisionCheck == ParentPin->GetOwningNode() || NodeToCollisionCheck == LastPin->GetOwningNode();
			if (!bSkipNode)
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("Checking collision for node %s | %f %f  | %f"), *FBAUtils::GetNodeName(NodeToCollisionCheck), TrackStart, TrackEnd, TestSolution);
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *NodeBounds.ToString());
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%f %f | %f"), TrackStart, TrackEnd, TestSolution);

				if (FBAUtils::LineRectIntersection(NodeBounds, StartPoint, EndPoint))
				{
					// UE_LOG(LogBlueprintAssist, Error, TEXT("\tNode collision  (%s) (%f) | %s"), *FBAUtils::GetNodeName(NodeToCollisionCheck), TestSolution, *NodeBounds.ToString());
					bNoCollisionInDirection = false;
					TestSolution = NodeBounds.Bottom + 1;
				}
			}
		}

		if (bNoCollisionInDirection)
		{
			TrackHeight = TestSolution;
			break;
		}
	}
}

bool FKnotNodeTrack::IsFloatingTrack() const
{
	const bool bSameAsParentPin = TrackHeight != FBAUtils::GetPinPos(GraphHandler, ParentPin).Y;
	const bool bSameAsLastPin = TrackHeight != FBAUtils::GetPinPos(GraphHandler, GetLastPin()).Y;
	return bSameAsParentPin && bSameAsLastPin;
}

void FKnotNodeTrack::UpdateTrackHeight(const float NewTrackY)
{
	const float Delta = NewTrackY - TrackHeight;

	for (TSharedPtr<FKnotNodeCreation> Creation : KnotCreations)
	{
		Creation->KnotPos.Y += Delta;
	}

	TrackHeight = NewTrackY;
}

TSet<UEdGraphNode*> FKnotNodeTrack::GetNodes(UEdGraph* Graph)
{
	TSet<UEdGraphNode*> OutNodes;
	OutNodes.Add(ParentPin->GetOwningNode());

	for (UEdGraphPin* Pin : LinkedTo)
	{
		if (UEdGraphPin* SafePin = FBAUtils::GetPinFromGraph(Pin, Graph))
		{
			OutNodes.Add(SafePin->GetOwningNode());
		}
	}

	return OutNodes;
}

bool FKnotNodeTrack::DoesTrackOverlapNode(UEdGraphNode* Node) const
{
	const FSlateRect Bounds = GetTrackBounds();

	return FBAUtils::LineRectIntersection(
		FBAUtils::GetNodeBounds(Node),
		Bounds.GetTopLeft(),
		Bounds.GetBottomRight());
}

bool FKnotNodeTrack::HasPinToAlignTo() const
{
	return PinToAlignTo.IsValid();
}

bool FKnotNodeTrack::TryAlignTrack(TSharedPtr<FEdGraphFormatter> Formatter, float TrackStart, float TrackEnd, float TestHeight)
{
	const float TrackSpacing = GetMutableDefault<UBASettings>()->BlueprintKnotTrackSpacing;

	UEdGraphPin* MyPin = ParentPin;
	UEdGraphPin* LastPin = GetLastPin();

	const TArray<UEdGraphNode*>& AllNodes = Formatter->GetFormattedNodes().Array();
	for (UEdGraphNode* NodeToCollisionCheck : AllNodes)
	{
		FSlateRect NodeBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, NodeToCollisionCheck).ExtendBy(FVector2D(0, TrackSpacing - 1));

		FVector2D StartPoint(TrackStart, TestHeight);
		FVector2D EndPoint(TrackEnd, TestHeight);

		const bool bSkipNode = NodeToCollisionCheck == MyPin->GetOwningNode() || NodeToCollisionCheck == LastPin->GetOwningNode();
		if (!bSkipNode)
		{
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("Checking collision for node %s | %f %f  | %f"), *FBAUtils::GetNodeName(NodeToCollisionCheck), TrackStart, TrackEnd, TestHeight);
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%s"), *NodeBounds.ToString());
			// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t%f %f | %f"), TrackStart, TrackEnd, TestHeight);

			if (FBAUtils::LineRectIntersection(NodeBounds, StartPoint, EndPoint))
			{
				// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\tCollision!"));
				return false;
			}
		}
	}

	// UE_LOG(LogBlueprintAssist, Warning, TEXT("No collision!"));
	return true;
}

FString FKnotNodeTrack::ToString()
{
	return FString::Printf(TEXT("%s (%f)"), *FBAUtils::GetPinName(ParentPin), GetTrackHeight());
}

//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

FFormatXInfo::FFormatXInfo(const FPinLink& InLink, TSharedPtr<FFormatXInfo> InParent)
	: Link(InLink)
	, Parent(InParent) {}

UEdGraphNode* FFormatXInfo::GetNode() const
{
	return Link.GetNode();
}

void FFormatXInfo::SetParent(TSharedPtr<FFormatXInfo> NewParent)
{
	if (Parent.IsValid())
	{
		Parent->Children.Remove(SharedThis(this));
	}

	if (NewParent.IsValid())
	{
		//const FString OldParent
		//	= Parent != nullptr
		//	? *FBlueprintAssistUtils::GetNodeName(Parent->GetNode())
		//	: FString("None");
		//UE_LOG(
		//	LogBlueprintAssist,
		//	Warning,
		//	TEXT("\tSet Parent for %s to %s (Old : %s)"),
		//	*FBlueprintAssistUtils::GetNodeName(GetNode()),
		//	*FBlueprintAssistUtils::GetNodeName(NewParent->GetNode()),
		//	*OldParent);

		NewParent->Children.Add(SharedThis(this));
	}

	Parent = NewParent;
}

TArray<UEdGraphNode*> FFormatXInfo::GetChildren(EEdGraphPinDirection Direction, bool bInitialDirectionOnly) const
{
	TArray<UEdGraphNode*> OutChildren;

	const auto& FilterByDirection = [Direction](TSharedPtr<FFormatXInfo> Info)
	{
		return Info->Link.GetDirection() == Direction || Direction == EGPD_MAX;
	};
	TArray<TSharedPtr<FFormatXInfo>> PendingInfo = Children.FilterByPredicate(FilterByDirection);

	while (PendingInfo.Num() > 0)
	{
		TSharedPtr<FFormatXInfo> CurrentInfo = PendingInfo.Pop();
		if (OutChildren.Contains(CurrentInfo->GetNode()))
		{
			break;
		}

		OutChildren.Push(CurrentInfo->GetNode());

		for (TSharedPtr<FFormatXInfo> Info : bInitialDirectionOnly ? CurrentInfo->Children : CurrentInfo->Children.FilterByPredicate(FilterByDirection))
		{
			PendingInfo.Push(Info);
		}
	}

	return OutChildren;
}

TArray<UEdGraphNode*> FFormatXInfo::GetImmediateChildren() const
{
	TArray<UEdGraphNode*> OutChildren;
	for (TSharedPtr<FFormatXInfo> Child : Children)
	{
		OutChildren.Add(Child->GetNode());
	}
	return OutChildren;
}

TArray<FPinLink> FFormatXInfo::GetChildrenAsLinks(EEdGraphPinDirection Direction) const
{
	TArray<FPinLink> OutLinks;
	for (TSharedPtr<FFormatXInfo> Child : Children)
	{
		if (Child->Link.GetDirection() == Direction)
		{
			OutLinks.Add(Child->Link);
		}
	}
	return OutLinks;
}

TSharedPtr<FFormatXInfo> FFormatXInfo::GetRootParent()
{
	TSet<TSharedPtr<FFormatXInfo>> Visited;
	TSharedPtr<FFormatXInfo> Next = SharedThis(this);
	while (Next->Parent.IsValid())
	{
		if (Visited.Contains(Next))
		{
			return nullptr;
		}
		Visited.Add(Next);

		Next = Next->Parent;
	}

	return Next;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

FCommentHandler::FCommentHandler(TSharedPtr<FBAGraphHandler> InGraphHandler)
{
	Init(InGraphHandler);
}

void FCommentHandler::Init(TSharedPtr<FBAGraphHandler> InGraphHandler)
{
	if (!InGraphHandler.IsValid())
	{
		return;
	}

	GraphHandler = InGraphHandler;

	ParentComments.Reset();
	CommentNodesContains.Reset();

	TArray<UEdGraphNode_Comment*> CommentNodes = FBAUtils::GetCommentNodesFromGraph(GraphHandler->GetFocusedEdGraph());

	CommentNodes.Sort([](UEdGraphNode_Comment& NodeA, UEdGraphNode_Comment& NodeB)
	{
		return NodeA.CommentDepth > NodeB.CommentDepth;
	});

	for (UEdGraphNode_Comment* Comment : CommentNodes)
	{
		TArray<UEdGraphNode*> NodesUnderComment = FBAUtils::GetNodesUnderComment(Comment);

		for (UEdGraphNode* EdGraphNode : NodesUnderComment)
		{
			CommentNodesContains.FindOrAdd(Comment).Add(EdGraphNode);
			ParentComments.FindOrAdd(EdGraphNode).Add(Comment);
		}
	}
}

FSlateRect FCommentHandler::GetCommentBounds(UEdGraphNode_Comment* CommentNode, UEdGraphNode* NodeAsking)
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

	auto ContainedNodesBounds = FBAUtils::GetCachedNodeArrayBounds(GraphHandler, NodesUnderComment);
	// UE_LOG(LogBlueprintAssist, Warning, TEXT("\t\t ContainedNodesBounds %s"), *ContainedNodesBounds.ToString());
	for (UEdGraphNode_Comment* CommentUnderComment : CommentNodesUnderComment)
	{
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

FSlateRect FCommentHandler::GetRelativeNodeBounds(UEdGraphNode* Node, UEdGraphNode* NodeAsking)
{
	TSet<UEdGraphNode*> OutRelativeNodes;
	return GetRelativeNodeBounds(Node, NodeAsking, OutRelativeNodes);
}

FSlateRect FCommentHandler::GetRelativeNodeBounds(UEdGraphNode* Node, UEdGraphNode* NodeAsking, TSet<UEdGraphNode*>& OutRelativeNodes)
{
	TArray<UEdGraphNode_Comment*> NodeComments;
	if (ParentComments.Contains(Node))
	{
		NodeComments = ParentComments[Node];
	}

	TArray<UEdGraphNode_Comment*> NodeAskingCommentNodes;
	if (ParentComments.Contains(NodeAsking))
	{
		NodeAskingCommentNodes = ParentComments[NodeAsking];
	}

	FSlateRect OutBounds = FBAUtils::GetCachedNodeBounds(GraphHandler, Node);

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

		// skip if 1. nodes are not linked 2. any nodes are not in our node tree 3. if pure node: then the parent must be in the comment box
		auto ThisCapture = this;
		const auto ShouldSkip = [&CommentNodeTree, &ThisCapture, &NodesUnderComment](UEdGraphNode* Node)
		{
			if (FBAUtils::IsCommentNode(Node) || FBAUtils::IsKnotNode(Node))
			{
				return false;
			}

			return !CommentNodeTree.Contains(Node);
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
				OutRelativeNodes.Add(NodeUnderComment);
			}
		}
	}

	return OutBounds;
}
