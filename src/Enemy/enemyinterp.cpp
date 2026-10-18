#include <Enemy/EnemyInterp.hpp>
#include <Strategic/LiveActor.hpp>

static void einGetLengthToMario(TSpcTypedInterp<TLiveActor>* interp,
                                u32 arg_num)
{
	interp->push();
}

static void einGetLengthToCurPathNode(TSpcTypedInterp<TLiveActor>* interp,
                                      u32 arg_num)
{
	interp->push();
}

static void einGoToRandomNextGraphNode(TSpcTypedInterp<TLiveActor>* interp,
                                       u32 arg_num)
{
	interp->push();
}

static void einWalkToCurPathNode(TSpcTypedInterp<TLiveActor>* interp,
                                 u32 arg_num)
{
	interp->push();
}

static void einSetGraph(TSpcTypedInterp<TLiveActor>* interp, u32 arg_num)
{
	interp->push();
}

void TEinBinary::initUserBuiltin()
{
	TSpcTypedBinary<TLiveActor>::initUserBuiltin();
	bindSystemDataToSymbol("setGraph", (uintptr_t)&einSetGraph);
	bindSystemDataToSymbol("walkToCurPathNode", (uintptr_t)&einWalkToCurPathNode);
	bindSystemDataToSymbol("goToRandomNextGraphNode",
	                       (uintptr_t)&einGoToRandomNextGraphNode);
	bindSystemDataToSymbol("getLengthToCurPathNode",
	                       (uintptr_t)&einGetLengthToCurPathNode);
	bindSystemDataToSymbol("getLengthToMario", (uintptr_t)&einGetLengthToMario);
}
