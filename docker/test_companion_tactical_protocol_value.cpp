#include "Companion/TacticalProtocol.h"
#include <cstdio>
using namespace Companion::Tactical;
int main()
{
    Envelope req; req.requestId=1; req.ownerGuid=2; req.botGuid=3; req.observationVersion=4; req.capabilityVersion=5; req.loginGeneration=6; req.orderGeneration=7; req.captureTimeMs=100;
    Envelope res=req; CapabilityCatalog cat; cat.version=5; cat.count=3; cat.abilityIds[0]=10; cat.abilityIds[1]=20; cat.abilityIds[2]=30;
    Candidate c[2]={{0,10,0,5100},{1,20,100,5100}};
    if (!Validate(req,res,c,2,cat,false,200).legal()) return 1;
    if (Validate(req,res,c,2,cat,true,200).legal()) return 2;
    c[1].abilityId=10; if (Validate(req,res,c,2,cat,false,200).reject != Reject::Duplicate) return 3;
    c[1].abilityId=99; if (Validate(req,res,c,2,cat,false,200).reject != Reject::Capability) return 4;
    c[1].abilityId=20; if (Validate(req,res,c,2,cat,false,2201).reject != Reject::Stale) return 5;
    Round r; if (!r.Submit(100,1) || r.Complete(2,true) || !r.Complete(1,true)) return 6;
    r.OwnerHold(); if (r.Submit(200,2)) return 7; r.Resume(); r.CapabilityChanged(); if (r.Submit(300,3)) return 8;
    std::puts("tactical protocol value tests: ALL OK"); return 0;
}
