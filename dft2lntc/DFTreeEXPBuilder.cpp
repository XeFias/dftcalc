/*
 * DFTreeEXPBuilder.cpp
 * 
 * Part of dft2lnt library - a library containing read/write operations for DFT
 * files in Galileo format and translating DFT specifications into Lotos NT.
 * 
 * @author Freark van der Berg and extended by Dennis Guck
 */

#include "DFTreeBCGNodeBuilder.h"
#include "DFTreeEXPBuilder.h"
#include "FileWriter.h"
#include "dft2lnt.h"
#include "automata/signals.h"

#include <map>
#include <fstream>

static const int VERBOSITY_FLOW = 1;
static const int VERBOSITY_RULES = 2;
static const int VERBOSITY_RULEORIGINS = 3;

std::ostream& operator<<(std::ostream& stream, const DFT::EXPSyncItem& item) {
	bool first = true;
	for(int a: item.args) {
		if(first) first = false;
		else stream << ",";
		stream << a;
	}
	return stream;
}

void DFT::DFTreeEXPBuilder::printSyncLineShort(std::ostream& stream, const EXPSyncRule& rule) {
	stream << "< ";
	bool first = true;
	for(auto syncIdx: rule.label) {
		if(first) first = false;
		else stream << " | ";
		if (syncIdx.first > 0) {
			const DFT::Nodes::Node* node = getNodeWithID(syncIdx.first);
			if(node)
				stream << node->getName();
			else
				stream << "error";
		} else {
			stream << "TopLevel";
		}
		stream << ":";
		stream << *syncIdx.second;
	}
	stream << " > @ " << (rule.syncOnNode?rule.syncOnNode->getName():"NOSYNC") << " -> " << rule.toLabel;
}

static std::string rename(bool comma, std::string gate, const decnumber<> &rate)
{
	std::string start(comma ? ", \"" : "\"");
	if (!rate.is_zero())
		return start + gate + "\" -> \"rate " + rate.str() + '"';
	else if (!comma) /* Return something to avoid a spurious comma */
		return start + gate + "\" -> \"" + gate + '"';
	else
		return "";
}

std::string DFT::DFTreeEXPBuilder::getBEProc(const DFT::Nodes::BasicEvent& be) const {
	std::stringstream ss;

	if(be.getMode() == DFT::Nodes::BE::CalculationMode::APH) {
		ss << "total rename ";
		ss << "\"ACTIVATE\" -> \"" << automata::signals::GATE_ACTIVATE << " !0 !FALSE\"";
		ss << ", ";
		ss << "\"FAIL\" -> \"" << automata::signals::GATE_FAIL << " !0\"";
		ss << " in \"";
		ss << be.getFileToEmbed();
		ss << "\" end rename";
	} else if (!be.getLambda().is_zero() || !be.getProb().is_zero()) {
		using namespace automata::signals;
		const decnumber<> ONE(1);
		decnumber<> l = be.getLambda(), mu = be.getMu();
		decnumber<> p = be.getProb();
		decnumber<> failSafe = 0;
		decnumber<> res = be.getRes(), cov = ONE - res;
		if ((double)l < 0) {
			/* Purely probabilistic BE. Assign arbitrary rate
			 * since only time-unbounded properties make sense
			 * anyway.
			 */
			l = ONE;
			failSafe = ONE - l*p;
		} else {
			failSafe = l * (ONE - p);
		}

		const std::string GATE = automata::signals::GATE_RATE_FAIL;
		ss << "total rename ";
		// Insert lambda value
		ss << rename(0, RATE_FAIL(1, 2), l * p * cov);
		ss << rename(1, RATE_FAIL(1, 4), l * p * res);
		ss << rename(1, RATE_FAIL(0, 2), failSafe * cov);
		ss << rename(1, RATE_FAIL(0, 4), failSafe * res);
		l = be.getLambda() * cov;
		res = be.getLambda() * res;
		for (int i = be.getPhases(); i > 1; i--) {
			ss << rename(1, RATE_FAIL(i, 2), l * res);
			ss << rename(1, RATE_FAIL(i, 4), l * cov);
		}
	
		// Insert mu value (only for non-cold BE's)
		failSafe = mu * (ONE - p);
		ss << rename(1, RATE_FAIL(1, 1), mu * p * cov);
		ss << rename(1, RATE_FAIL(1, 3), mu * p * res);
		ss << rename(1, RATE_FAIL(1, 1), failSafe * cov);
		ss << rename(1, RATE_FAIL(1, 3), failSafe * res);
		for (int i = be.getPhases(); i > 1; i--) {
			ss << rename(1, RATE_FAIL(i, 1), mu * cov);
			ss << rename(1, RATE_FAIL(i, 3), mu * res);
		}
		if (be.getRepair()>0) {
			ss << ", ";
			ss << "\"" << automata::signals::GATE_RATE_REPAIR << "\" -> \"rate " << be.getRepair() << "\"";
		}
		ss << " in \"";
		ss << nodeBuilder->getRoot() << nodeBuilder->getFileForNode(be);
		ss << "\" end rename";
	} else {
		ss << "\"";
		ss << nodeBuilder->getRoot() << nodeBuilder->getFileForNode(be);
		ss << "\"";
	}
	return ss.str();
}

std::string DFT::DFTreeEXPBuilder::getRUProc(const DFT::Nodes::Gate& ru) const {
	std::stringstream ss;

	ss << "total rename ";

	for(size_t n = 0; n<ru.getChildren().size(); ++n) {

		// Get the current child and associated childID
		const DFT::Nodes::Node& child = *ru.getChildren().at(n);

		if(child.isBasicEvent()) {
			const DFT::Nodes::BasicEvent& be = *static_cast<const DFT::Nodes::BasicEvent*>(&child);

			// Insert repair values
			ss << "\"" << automata::signals::GATE_RATE_REPAIR << " !1 !" << n+1 << "\" -> \"rate " << be.getRepair() << "\"";
			if(n < ru.getChildren().size()-1){
				ss << ", ";
			}
		}

	}

	ss << " in \"";
	ss << nodeBuilder->getRoot() << nodeBuilder->getFileForNode(ru);
	ss << "\" end rename";

	return ss.str();
}

std::string DFT::DFTreeEXPBuilder::getINSPProc(const DFT::Nodes::Inspection& insp) const {
    std::stringstream ss;
    
    ss << "total rename ";
    // Insert lambda value
	std::string rate = "rate " + insp.getLambda().str();
	if (insp.getPhases() == 0)
		rate = "time " + insp.getLambda().str();
    ss << "\"" << automata::signals::GATE_RATE_INSPECTION << " !1" << "\" -> \"" << rate << "\"";
    
    ss << " in \"";
    ss << nodeBuilder->getRoot() << nodeBuilder->getFileForNode(insp);
    ss << "\" end rename";
    
    return ss.str();
}

std::string DFT::DFTreeEXPBuilder::getREPProc(const DFT::Nodes::Replacement& rep) const {
    std::stringstream ss;
    
    ss << "total rename ";
    // Insert lambda value
    ss << "\"" << automata::signals::GATE_RATE_PERIOD << " !1 !" << "\" -> \"rate " << rep.getLambda() << "\"";
    
    ss << " in \"";
    ss << nodeBuilder->getRoot() << nodeBuilder->getFileForNode(rep);
    ss << "\" end rename";
    
    return ss.str();
}

void DFT::DFTreeEXPBuilder::printSyncLine(const EXPSyncRule& rule, const vector<unsigned int>& columnWidths) {
	std::map<unsigned int,std::shared_ptr<EXPSyncItem>>::const_iterator it = rule.label.begin();
	size_t c=0;
	for(; it!=rule.label.end();++it) {
		while(c<it->first) {
			if(c>0) exp_body << " * ";
			exp_body.outlineLeftNext(columnWidths[c],' ');
			exp_body << "_";
			++c;
		}
		if(c>0) exp_body << " * ";
		exp_body.outlineLeftNext(columnWidths[c],' ');
		exp_body << it->second->toStringQuoted();
		++c;
	}
	while(c<dft->getNodes().size() + 1) {
		if(c>0) exp_body << " * ";
		exp_body.outlineLeftNext(columnWidths[c],' ');
		exp_body << "_";
		++c;
	}
	if (rule.toLabel.find(' ') == std::string::npos)
		exp_body << " -> " << rule.toLabel;
	else
		exp_body << " -> \"" << rule.toLabel << "\"";
}

DFT::DFTreeEXPBuilder::DFTreeEXPBuilder(std::string root, std::string tmp, std::string nameBCG, std::string nameEXP, DFT::DFTree* dft, DFTreeNodeBuilder *nb, CompilerContext* cc):
	root(root),
	tmp(tmp),
	nameBCG(nameBCG),
	nameEXP(nameEXP),
	nameTop(""),
	dft(dft),
	cc(cc),
	nodeBuilder(nb)
{ }

int DFT::DFTreeEXPBuilder::build() {

	bool ok = true;
	
	// Check all the nodes in the DFT, adding BasicEvents to basicEvents and
	// Gates to gates. Also keep track of what Lotos NT files are needed by
	// adding them to neededFiles.
	basicEvents.clear();
	gates.clear();
	nodeIDs.clear();
	for(size_t i=0; i<dft->getNodes().size(); ++i) {
		DFT::Nodes::Node* node = dft->getNodes().at(i);
		if(DFT::Nodes::Node::typeMatch(node->getType(),DFT::Nodes::BasicEventType)) {
			DFT::Nodes::BasicEvent* be = static_cast<DFT::Nodes::BasicEvent*>(node);
			basicEvents.push_back(be);
		} else if(DFT::Nodes::Node::typeMatch(node->getType(),DFT::Nodes::GateType)) {
			DFT::Nodes::Gate* gate = static_cast<DFT::Nodes::Gate*>(node);
			gates.push_back(gate);
		} else {
			cc->reportErrorAt(node->getLocation(),"DFTreeEXPBuilder cannot handles this node");
			ok = false;
		}
		nodeIDs.insert(pair<const DFT::Nodes::Node*, unsigned int>(node, i + 1));
	}

	if(ok) {
		// Build the EXP file
		svl_header.clearAll();
		svl_body.clearAll();
		exp_header.clearAll();
		exp_body.clearAll();
		
		vector<DFT::EXPSyncRule> activationRules;
		vector<DFT::EXPSyncRule> failRules;
		// new rules for repair, repaired and online
		vector<DFT::EXPSyncRule> repairRules;
		vector<DFT::EXPSyncRule> repairedRules;
		vector<DFT::EXPSyncRule> repairingRules;
		vector<DFT::EXPSyncRule> onlineRules;
        // extra inspection rules
        vector<DFT::EXPSyncRule> inspectionRules;
		// Rules for detection of modelling errors.
        vector<DFT::EXPSyncRule> impossibleRules;

        parseDFT(impossibleRules, activationRules,failRules,repairRules,repairedRules,repairingRules,onlineRules,inspectionRules);
        buildEXPBody(impossibleRules, activationRules,failRules,repairRules,repairedRules,repairingRules,onlineRules,inspectionRules);

		// Build SVL file
		svl_body << "%EXP_OPEN_OPTIONS=\"-rate\";" << svl_body.applypostfix;
		svl_body << "%BCG_MIN_OPTIONS=\"-rate -self -epsilon 5e-324\";" << svl_body.applypostfix;
		svl_body << "\"" << nameBCG << "\" = smart stochastic branching reduction of \"" << nameEXP << "\";" << svl_body.applypostfix;
	}
	return !ok;
}

void DFT::DFTreeEXPBuilder::printEXP(std::ostream& out) {
	out << exp_header.toString();
	out << exp_body.toString();
}

void DFT::DFTreeEXPBuilder::printSVL(std::ostream& out) {
	out << svl_header.toString();
	out << svl_body.toString();
}

unsigned int DFT::DFTreeEXPBuilder::getIDOfNode(const DFT::Nodes::Node& node) const {
	std::map<const DFT::Nodes::Node*, unsigned int>::const_iterator it = nodeIDs.find(&node);
	assert( (it != nodeIDs.end()) && "getIDOfNode() did not know the ID of the node");
	
	return it->second;
}

int DFT::DFTreeEXPBuilder::getLocalIDOfNode(const DFT::Nodes::Node* parent, const DFT::Nodes::Node* child) const {
	if(parent->isGate()) {
		const DFT::Nodes::Gate* gate = static_cast<const DFT::Nodes::Gate*>(parent);
		int i=(int)gate->getChildren().size(); 
		for(;i--;) {
			if(gate->getChildren()[i]==child) {
				return i;
			}
		}
	}
	return -1;
}

const DFT::Nodes::Node* DFT::DFTreeEXPBuilder::getNodeWithID(unsigned int id) {
	assert(0 < id && id <= dft->getNodes().size());
	return dft->getNodes().at(id - 1);
}

int DFT::DFTreeEXPBuilder::parseDFT(
			vector<DFT::EXPSyncRule>& impossibleRules,
			vector<DFT::EXPSyncRule>& activationRules,
			vector<DFT::EXPSyncRule>& failRules,
			vector<DFT::EXPSyncRule>& repairRules,
			vector<DFT::EXPSyncRule>& repairedRules,
			vector<DFT::EXPSyncRule>& repairingRules,
			vector<DFT::EXPSyncRule>& onlineRules,
			vector<DFT::EXPSyncRule>& inspectionRules)
{
    
    /* Create synchronization rules for all nodes in the DFT */
    
    // Create synchronization rules for the Top node in the DFT
    createSyncRuleTop(activationRules,failRules,onlineRules);
    
    // Create synchronization rules for all nodes in the DFT
    {
        std::vector<DFT::Nodes::Node*>::const_iterator it = dft->getNodes().begin();
        for(;it!=dft->getNodes().end();++it) {
            if((*it)->isGate()) {
                const DFT::Nodes::Gate& gate = static_cast<const DFT::Nodes::Gate&>(**it);
                cc->reportAction("Creating synchronization rules for `" + gate.getName() + "' (THIS node)",VERBOSITY_FLOW);
                createSyncRule(impossibleRules, activationRules,failRules,repairRules,repairedRules,repairingRules,onlineRules,inspectionRules,gate,getIDOfNode(gate));
            } else {
				const DFT::Nodes::BasicEvent& be = static_cast<const DFT::Nodes::BasicEvent&>(**it);
				if (!be.hasInspectionModule()) {
					addIndepRule(inspectionRules, be, syncInspection(0), "i_");
				} else if (be.getRepair() <= 0) {
					EXPSyncItem *RR = new EXPSyncItem(
								automata::signals::GATE_RATE_REPAIR);
					addIndepRule(repairRules, be, RR, "rr_");
				}
			}
			unsigned int nodeID = getIDOfNode(**it);
			/* Impossible (model-error) rule */
			std::string iName("IMPOSSIBLE_");
			iName += (*it)->getTypeStr();
			iName += std::to_string(nodeID);
			DFT::EXPSyncRule ruleI(iName, false);
			ruleI.insertLabel(nodeID, syncImpossible());
			impossibleRules.push_back(ruleI);
        }
    }
    return 0;
}

void DFT::DFTreeEXPBuilder::writeHideLines(vector<DFT::EXPSyncRule>& rules)
{
	bool first = true;
	for(size_t s = 0; s < rules.size(); ++s) {
		EXPSyncRule& rule = rules[s];
		if(rule.hideToLabel) {
			if (!first)
				exp_body << "," << exp_body.applypostfix;
			first = false;
			exp_body << exp_body.applyprefix << rule.toLabel;
		}
	}
	exp_body << exp_body.applypostfix;
}

void DFT::DFTreeEXPBuilder::writeRules(vector<DFT::EXPSyncRule>& rules,
									   vector<unsigned int> columnWidths)
{
	bool first = true;
	for(size_t s = 0; s < rules.size(); ++s) {
		if (!first)
			exp_body << "," << exp_body.applypostfix;
		first = false;
		exp_body << exp_body.applyprefix;
		printSyncLine(rules[s],columnWidths);
	}
	exp_body << exp_body.applypostfix;
}

int DFT::DFTreeEXPBuilder::buildEXPBody(
			vector<DFT::EXPSyncRule>& impossibleRules,
			vector<DFT::EXPSyncRule>& activationRules,
			vector<DFT::EXPSyncRule>& failRules,
			vector<DFT::EXPSyncRule>& repairRules,
			vector<DFT::EXPSyncRule>& repairedRules,
			vector<DFT::EXPSyncRule>& repairingRules,
			vector<DFT::EXPSyncRule>& onlineRules,
			vector<DFT::EXPSyncRule>& inspectionRules)
{

	vector<DFT::EXPSyncRule> allRules(activationRules);
	allRules.insert(allRules.end(), repairRules.begin(), repairRules.end());
	allRules.insert(allRules.end(), repairedRules.begin(), repairedRules.end());
	allRules.insert(allRules.end(), repairingRules.begin(), repairingRules.end());
	allRules.insert(allRules.end(), onlineRules.begin(), onlineRules.end());
	allRules.insert(allRules.end(), inspectionRules.begin(), inspectionRules.end());
	allRules.insert(allRules.end(), failRules.begin(), failRules.end());
	allRules.insert(allRules.end(), impossibleRules.begin(), impossibleRules.end());

    /* Generate the EXP based on the generated synchronization rules */
    exp_body.clearAll();
    exp_body << exp_body.applyprefix << "(* Number of rules: " << allRules.size();
   	exp_body << "*)" << exp_body.applypostfix;
    exp_body << exp_body.applyprefix << "hide" << exp_body.applypostfix;
    exp_body.indent();
    
	writeHideLines(allRules);
    
    exp_body.outdent();
    exp_body.appendLine("in");
    exp_body.indent();

	// Synchronization rules
	vector<unsigned int> columnWidths(dft->getNodes().size() + 1, 0);
	calculateColumnWidths(columnWidths,allRules);
	
	exp_body << exp_body.applyprefix << "label par using" << exp_body.applypostfix;
	exp_body << exp_body.applyprefix << "(*\t";
	exp_body.outlineLeftNext(columnWidths[0],' ');
	exp_body << exp_body._push << "tle" << exp_body._pop;

	std::vector<DFT::Nodes::Node*>::const_iterator it = dft->getNodes().begin();
	for(int c = 0; it!=dft->getNodes().end(); ++it,++c) {
		exp_body << "   ";
		exp_body.outlineLeftNext(columnWidths[c + 1],' ');
		exp_body << exp_body._push << (*it)->getTypeStr() << getIDOfNode(**it)
			<< exp_body._pop;
	}
	exp_body << " *)" << exp_body.applypostfix;
	
	exp_body.indent();
	writeRules(allRules, columnWidths);
	exp_body.outdent();
	
	// Generate the parallel composition of all the nodes
	exp_body << exp_body.applyprefix << "in" << exp_body.applypostfix;
	exp_body.indent();
	exp_body << exp_body.applyprefix;
	exp_body << "\"" << nodeBuilder->getRoot() << nodeBuilder->getFileForTopLevel() << "\"";
	exp_body << exp_body.applypostfix;
	int c=0;
	it = dft->getNodes().begin();
	for(it = dft->getNodes().begin(); it!=dft->getNodes().end(); ++it, ++c) {
		const DFT::Nodes::Node& node = **it;
		exp_body << exp_body.applyprefix << "||" << exp_body.applypostfix;
		if(node.isBasicEvent()) {
			const DFT::Nodes::BasicEvent *be;
		   	be = static_cast<const DFT::Nodes::BasicEvent*>(&node);
			exp_body << exp_body.applyprefix << getBEProc(*be)
			         << exp_body.applypostfix;
		} else if (node.isGate()) {
			if (DFT::Nodes::Node::typeMatch(node.getType(),
			                                DFT::Nodes::InspectionType))
			{
				const DFT::Nodes::Inspection *insp;
			   	insp = static_cast<const DFT::Nodes::Inspection*>(&node);
				exp_body << exp_body.applyprefix << getINSPProc(*insp)
				         << exp_body.applypostfix;
			} else if (DFT::Nodes::Node::typeMatch(node.getType(),
												   DFT::Nodes::ReplacementType))
			{
				const DFT::Nodes::Replacement *rep;
			   	rep = static_cast<const DFT::Nodes::Replacement*>(&node);
				exp_body << exp_body.applyprefix << getREPProc(*rep)
				         << exp_body.applypostfix;
			} else {
				exp_body << exp_body.applyprefix << "\""
				         << nodeBuilder->getRoot()
				         << nodeBuilder->getFileForNode(node)
				         << "\"" << exp_body.applypostfix;
			}
		} else {
			assert(0 && "buildEXPBody(): Unknown node type");
		}
	}
	exp_body.outdent();
	exp_body << exp_body.applyprefix << "end par" << exp_body.applypostfix;
    exp_body.outdent();
    exp_body.appendLine("end hide");
    return 0;
}
	
/**
 * Add FDEP Specific syncRules
 * The FDEP needs to synchronize with multiple nodes: its source (trigger) and its dependers.
 * To synchronize with the source, the general parent-child relationship is used. But for the rest, we need
 * this method. This method will add a failSyncRule per depender to the list of rules, such that each
 * dependers' parents will be notified, in a nondeterministic fashion. Thus, if the source fails, the order
 * of failing dependers is nondeterministic.
 */
int DFT::DFTreeEXPBuilder::createSyncRuleGateFDEP(vector<DFT::EXPSyncRule>& activationRules, vector<DFT::EXPSyncRule>& failRules, const DFT::Nodes::GateFDEP& node, unsigned int nodeID) {

	// Loop over all the dependers
	cc->reportAction3("FDEP Dependencies of THIS node...",VERBOSITY_RULEORIGINS);
	for(int dependerLocalID=0; dependerLocalID<(int)node.getDependers().size(); ++dependerLocalID) {
		DFT::Nodes::Node* depender = node.getDependers()[dependerLocalID];
		unsigned int dependerID = getIDOfNode(*depender);

		cc->reportAction3("Depender `" + depender->getName() + "'",VERBOSITY_RULEORIGINS);

		// Create a new failSyncRule
		std::stringstream ss;
		ss << "f_" << node.getTypeStr() << nodeID << "_" << depender->getTypeStr() << dependerID;
		EXPSyncRule ruleF(ss.str());

		// Add the depender to the synchronization (+2, because in LNT the depender list starts at 2)
		ruleF.insertLabel(nodeID, syncFail(dependerLocalID + 2));
		cc->reportAction3("Added fail rule, notifying:",VERBOSITY_RULEORIGINS);

		// Loop over the parents of the depender
		for(DFT::Nodes::Node* depParent: depender->getParents()) {
			// Obtain the localChildID of the depender seen from this parent
			unsigned int parentID = getIDOfNode(*depParent);
			int localChildID = getLocalIDOfNode(depParent,depender);
			assert(localChildID>=0 && "depender is not a child of its parent");

			// Add the parent to the synchronization rule, hooking into the FAIL of the depender,
			// making it appear to the parent that the child failed (+1, because in LNT the child list starts at 1)
			ruleF.insertLabel(parentID, syncFail(localChildID + 1));
			cc->reportAction3("  Node `" + depParent->getName() + "'",VERBOSITY_RULEORIGINS);
		}

		// Add it to the list of rules
		failRules.push_back(ruleF);
		{
			std::stringstream report;
			report << "Added new fail       sync rule: ";
			printSyncLineShort(report,ruleF);
			cc->reportAction2(report.str(),VERBOSITY_RULES);
		}
	}

	return 0;
}

int DFT::DFTreeEXPBuilder::createSyncRuleTop(
		vector<DFT::EXPSyncRule>& activationRules,
		vector<DFT::EXPSyncRule>& failRules,
		vector<DFT::EXPSyncRule>& onlineRules)
{
	std::stringstream ss;

	unsigned int topNode = nodeIDs[dft->getTopNode()];

	// Generate the Top Node Activate rule
	ss << automata::signals::GATE_ACTIVATE;
	if(!nameTop.empty())
		ss << "_" << nameTop;
	DFT::EXPSyncRule ruleA(ss.str(), true);

	ruleA.insertLabel(topNode, syncActivate(0, false));
	ruleA.insertLabel(0, syncActivate(0, true));

	// Generate the FDEP Node Activate rule
	int c=0;
	std::vector<DFT::Nodes::Node*>::iterator it;
	for(it = dft->getNodes().begin(); it!=dft->getNodes().end();++it,++c) {
		const DFT::Nodes::Node& node = **it;
		if(node.isGate()) {
			if(node.matchesType(DFT::Nodes::GateFDEPType)) {
				ruleA.insertLabel(c, syncActivate(0, false));
			}
		}
	}

	std::stringstream report;
	report << "New EXPSyncRule " << ss.str();
	cc->reportAction3(report.str(),VERBOSITY_RULEORIGINS);

	// Generate the Top Node Fail rule
	ss.str(std::string());
	ss << automata::signals::GATE_FAIL;
	if(!nameTop.empty())
		ss << "_" << nameTop;
	DFT::EXPSyncRule ruleF(ss.str(),false);
	ruleF.syncOnNode = dft->getTopNode();
	ruleF.insertLabel(topNode, syncFail(0));

	cc->reportAction("Creating synchronization rules for Top node",VERBOSITY_FLOW);
	if(dft->getTopNode()->isRepairable()){
		// Generate the Top Node Online rule
		ss.str("");
		ss << automata::signals::GATE_ONLINE;
		DFT::EXPSyncRule ruleO(ss.str(),false);
		if(!nameTop.empty())
			ss << "_" << nameTop;
		ruleO.insertLabel(topNode, syncOnline(0));
		onlineRules.push_back(ruleO);
		ss.str("Added new online     sync rule: ");
		printSyncLineShort(ss, ruleO);
		cc->reportAction2(ss.str(), VERBOSITY_RULES);
	}

	// Add the generated rules to the lists
	activationRules.push_back(ruleA);
	failRules.push_back(ruleF);

	ss.str("Added new activation sync rule: ");
	printSyncLineShort(ss, ruleA);
	cc->reportAction2(ss.str(), VERBOSITY_RULES);

	ss.str("Added new fail       sync rule: ");
	printSyncLineShort(ss, ruleF);
	cc->reportAction2(ss.str(), VERBOSITY_RULES);

	return 0;
}

/** Add a rule between a child and any of its parents. */
DFT::EXPSyncRule &DFT::DFTreeEXPBuilder::addAnycastRule(
		vector<DFT::EXPSyncRule> &rules,
		const DFT::Nodes::Gate &node,
		EXPSyncItem *nodeSignal,
		EXPSyncItem *childSignal,
		std::string name_prefix,
		unsigned int childNum)
{
	const DFT::Nodes::Node &child = *node.getChildren().at(childNum);
	unsigned int childID = nodeIDs[&child];
	unsigned int nodeID = nodeIDs[&node];

	std::stringstream ss;
	ss << name_prefix << node.getTypeStr() << nodeID << '_' << child.getTypeStr() << childID;
	EXPSyncRule rule(ss.str());
	rule.syncOnNode = &child;
	rule.insertLabel(nodeID, nodeSignal);
	rule.insertLabel(childID, childSignal);
	std::stringstream report("Added new anycast sync rule: ");
	printSyncLineShort(report, rule);
	cc->reportAction3(report.str(), VERBOSITY_RULES);
	rules.push_back(rule);
	return rules[rules.size() - 1];
}

/** Add broadcast from a parent to all of its children */
void DFT::DFTreeEXPBuilder::addInvBroadcastRule(vector<DFT::EXPSyncRule> &rules,
											 const DFT::Nodes::Gate &node,
											 EXPSyncItem *nodeSignal,
											 EXPSyncItem *childSignal,
											 std::string name_prefix,
											 unsigned int childNum)
{
	const DFT::Nodes::Node *child = node.getChildren().at(childNum);
	unsigned int nodeID = nodeIDs[&node];
	for (auto &rule : rules) {
		// If there is a rule that also synchronizes on the same node,
		// we have come across a different child of this node.
		if(rule.syncOnNode == &node) {
			shared_ptr<EXPSyncItem> prevSignal = rule.label[nodeID];
			if (prevSignal->toString() != nodeSignal->toString())
				continue;
			cc->reportAction3("Found earlier fail rule",VERBOSITY_RULEORIGINS);
			rule.insertLabel(nodeIDs[child], childSignal);
			return;
		}
	}

	EXPSyncRule &rule = addAnycastRule(rules, node, nodeSignal, childSignal, name_prefix, childNum);
	rule.syncOnNode = &node;
}

/** Add a rule broadcast from a child to all its parents. */
void DFT::DFTreeEXPBuilder::addBroadcastRule(vector<DFT::EXPSyncRule> &rules,
					     const DFT::Nodes::Gate &node,
					     EXPSyncItem *nodeSignal,
					     EXPSyncItem *childSignal,
					     std::string name_prefix,
					     unsigned int childNum)
{
	const DFT::Nodes::Node *child = node.getChildren().at(childNum);
	unsigned int nodeID = nodeIDs[&node];
	for (auto &rule : rules) {
		// If there is a rule that also synchronizes on the same node,
		// we have come across a child with another parent.
		if(rule.syncOnNode == child) {
			shared_ptr<EXPSyncItem> prevSignal = rule.label[nodeIDs[child]];
			if (prevSignal->toString() != childSignal->toString())
				continue;
			cc->reportAction3("Found earlier fail rule",VERBOSITY_RULEORIGINS);
			rule.insertLabel(nodeID, nodeSignal);
			return;
		}
	}

	addAnycastRule(rules, node, nodeSignal, childSignal, name_prefix, childNum);
}

/** Add a rule to make a given signal independent */
void DFT::DFTreeEXPBuilder::addIndepRule(vector<DFT::EXPSyncRule> &rules,
					     const DFT::Nodes::Node &node,
					     EXPSyncItem *nodeSignal,
					     std::string name_prefix)
{
	unsigned int nodeID = nodeIDs[&node];

	std::stringstream ss;
	ss << name_prefix << '_' << node.getTypeStr() << nodeID;
	EXPSyncRule rule(ss.str());
	rule.syncOnNode = &node;
	rule.insertLabel(nodeID, nodeSignal);
	std::stringstream report("Added new independent sync rule: ");
	printSyncLineShort(report, rule);
	cc->reportAction2(report.str(),VERBOSITY_RULES);
	rules.push_back(rule);
}

int DFT::DFTreeEXPBuilder::createSyncRule(
			vector<DFT::EXPSyncRule>& impossibleRules,
			vector<DFT::EXPSyncRule>& activationRules,
			vector<DFT::EXPSyncRule>& failRules,
			vector<DFT::EXPSyncRule>& repairRules,
			vector<DFT::EXPSyncRule>& repairedRules,
			vector<DFT::EXPSyncRule>& repairingRules,
			vector<DFT::EXPSyncRule>& onlineRules,
			vector<DFT::EXPSyncRule>& inspectionRules,
			const DFT::Nodes::Gate& node,
			unsigned int nodeID)
{
	// Go through all the children
	for(size_t n = 0; n<node.getChildren().size(); ++n) {
		// Get the current child and associated childID
		const DFT::Nodes::Node *child = node.getChildren().at(n);
		unsigned int childID = nodeIDs[child];

		cc->reportAction2("Child `" + child->getName() + "'"
						  + (child->usesDynamicActivation()?" (dynact)":"")
						  + " ...",
						  VERBOSITY_RULES);

		// ask if we have a repair unit (if it is the case we don't have to handle activation and fail) same for inspection and replacement
		if (!node.matchesType(DFT::Nodes::RepairUnitAnyType)
			&& !node.matchesType(DFT::Nodes::InspectionType)
			&& !node.matchesType(DFT::Nodes::ReplacementType))
		{
			/** ACTIVATION RULE **/
			std::stringstream ss;
			ss << node.getTypeStr() << nodeID << "_" << child->getTypeStr()
			   << childID;
			EXPSyncRule ruleA("a_" + ss.str());
			EXPSyncRule ruleD("d_" + ss.str());

			std::stringstream report;
			report << "New EXPSyncRule " << ss.str();
			cc->reportAction3(report.str(),VERBOSITY_RULEORIGINS);

			// Set synchronization node
			ruleA.syncOnNode = child;
			ruleD.syncOnNode = child;

			// Add synchronization of THIS node to the synchronization rule
			ruleA.insertLabel(nodeID, syncActivate(n+1, true));
			ruleD.insertLabel(nodeID, syncDeactivate(n+1, true));
			cc->reportAction3("THIS node added to sync rule",
							  VERBOSITY_RULEORIGINS);

			// Go through all the existing activation rules
			for (auto &otherRule : activationRules) {
				// If there is a rule that also synchronizes on the same node,
				// we have come across a child with another parent.
				if(otherRule.syncOnNode != child)
					continue;
				std::stringstream report;
				report << "Detected earlier activation rule: ";
				printSyncLineShort(report, otherRule);
				cc->reportAction3(report.str(), VERBOSITY_RULEORIGINS);

				// First, we look up the sending Node of the
				// current activation rule...
				int otherNodeID = -1;
				int otherLocalNodeID = -1;
				for(auto& syncItem: otherRule.label) {
					if(syncItem.second->getArg(1)) {
						otherNodeID = syncItem.first;
						otherLocalNodeID = syncItem.second->getArg(0);
						break;
					}
				}
				if(otherNodeID<0) {
					cc->reportError("Could not find sender of the other rule, bailing...");
					return 1;
				}
				const DFT::Nodes::Node* otherNode = getNodeWithID(otherNodeID);
				assert(otherNode);

				// The synchronization depends if THIS node uses
				// dynamic activation or not. The prime example of
				// this is the Spare gate.
				// It also depends on the sending Node of the current
				// activation rule (otherNode). Both have to use
				// dynamic activation, because otherwise a Spare gate
				// will not activate a child if it's activated by a
				// static node, which is what is required.
				// The fundamental issue is that the semantics of
				// "using" or "claiming" a node is implicitely within
				// activating it. This should be covered more
				// thoroughly in the theory first, thus:
				/// @todo Fix "using" semantics in activation"
				if(node.usesDynamicActivation()
				   && otherNode->usesDynamicActivation())
				{
					// If the THIS node used dynamic activation, the
					// node is activated by an other parent.
					// Thus, add a synchronization item to the
					// existing synchronization rule, specifying that
					// the THIS node also receives a sent Activate.
					if (otherRule.toLabel[0] == 'd')
						otherRule.insertLabel(nodeID, syncDeactivate(n+1, false));
					else
						otherRule.insertLabel(nodeID, syncActivate(n+1, false));
					cc->reportAction3("THIS node added, activation listening synchronization",VERBOSITY_RULEORIGINS);

					// This is not enough, because the other way
					// around also has to be added: the other node
					// wants to listen to Activates of the THIS node
					// as well.
					// Thus, we add a synchronization item to the
					// new rule we create for the THIS node, specifying
					// the other node wants to listen to activates of
					// the THIS node.
					ruleA.insertLabel(otherNodeID, syncActivate(otherLocalNodeID, false));
					ruleD.insertLabel(otherNodeID, syncDeactivate(otherLocalNodeID, false));
					cc->reportAction3("Detected (other) dynamic activator `" + otherNode->getName() + "', added to sync rule",VERBOSITY_RULEORIGINS);

					// TODO: primary is a special case??????
				}
			}

			// Add the child Node to the synchronization rule
			// Create synchronization rules a_<nodetype><nodeid>_<childtype><childid>
			ruleA.insertLabel(childID, syncActivate(0, false));
			ruleD.insertLabel(childID, syncDeactivate(0, false));
			cc->reportAction3("Child added to sync rule",VERBOSITY_RULEORIGINS);
			report.str("Added new activation sync rule: ");
			printSyncLineShort(report,ruleA);
			cc->reportAction2(report.str(),VERBOSITY_RULES);
			activationRules.push_back(ruleA);
			activationRules.push_back(ruleD);

			addBroadcastRule(failRules, node, syncFail(n + 1), syncFail(0),
							 "f_", n);
			/** ONLINE Rules **/
			if (node.isRepairable()) {
				addBroadcastRule(onlineRules, node, syncOnline(n + 1),
								 syncOnline(0), "o_", n);
			}
		} else if (!node.matchesType(DFT::Nodes::InspectionType)
				   && !node.matchesType(DFT::Nodes::ReplacementType))
		{
			addBroadcastRule(repairRules, node, syncRepair(n + 1),
							 syncRepair(true), "rep_", n);
			addBroadcastRule(repairedRules, node, syncRepaired(n + 1),
							 syncRepaired(0), "repd_", n);
			// Special case: If any other repair unit wants to repair
			// our child, we should be informed so we don't try to
			// repair the same child.

			/** ACTIVATION RULE **/
			std::stringstream ss;
			ss << node.getTypeStr() << nodeID << "_" << child->getTypeStr()
			   << childID;
			EXPSyncRule rule("repi_" + ss.str());

			std::stringstream report;
			report << "New EXPSyncRule " << ss.str();
			cc->reportAction3(report.str(),VERBOSITY_RULEORIGINS);

			// Set synchronization node
			rule.syncOnNode = child;

			// Add synchronization of THIS node to the synchronization rule
			rule.insertLabel(nodeID, syncRepairing(n+1, true));
			rule.insertLabel(childID, syncRepairing(0));
			cc->reportAction3("THIS node added to sync rule",
							  VERBOSITY_RULEORIGINS);
			for (auto &otherRule : repairingRules) {
				// If there is a rule that also synchronizes on the same node,
				// we have come across a child with another parent.
				if(otherRule.syncOnNode != child)
					continue;
				std::stringstream report;
				report << "Detected earlier repairing rule: ";
				printSyncLineShort(report, otherRule);
				cc->reportAction3(report.str(), VERBOSITY_RULEORIGINS);

				// First, we look up the sending Node of the
				// current repairing rule...
				int otherNodeID = -1;
				int otherLocalNodeID = -1;
				for(auto& syncItem: otherRule.label) {
					if (syncItem.second->args.size() == 1)
						continue;
					std::cerr << "Looking up " << syncItem.second->toString() << "\n";
					if(syncItem.second->getArg(1)) {
						otherNodeID = syncItem.first;
						otherLocalNodeID = syncItem.second->getArg(0);
						break;
					}
				}
				if(otherNodeID<0) {
					cc->reportError("Could not find sender of the other rule, bailing...");
					return 1;
				}
				const DFT::Nodes::Node* otherNode = getNodeWithID(otherNodeID);
				assert(otherNode);

				otherRule.insertLabel(nodeID, syncRepairing(n+1, false));

				// This is not enough, because the other way around also
				// has to be added: the other node wants to listen to
				// Repairing of the THIS node as well.
				// Thus, we add a synchronization item to the
				// new rule we create for the THIS node, specifying
				// the other node wants to listen to repairing of
				// the THIS node.
				rule.insertLabel(otherNodeID, syncRepairing(otherLocalNodeID, false));
				cc->reportAction3("Detected (other) repair unit `" + otherNode->getName() + "', added to sync rule",VERBOSITY_RULEORIGINS);

			}
			repairingRules.push_back(rule);
		} else if(node.matchesType(DFT::Nodes::InspectionType)) {
			addBroadcastRule(inspectionRules, node, syncInspection(n+1),
							 syncInspection(0), "insp_", n);
			addBroadcastRule(failRules, node, syncInspection(n+1),
							 syncFail(0), "inspf_", n);
			/* Don't send repair signals to gates */
			if (!child->matchesType(DFT::Nodes::GateType)) {
				addInvBroadcastRule(repairRules, node, syncRepair((size_t)0),
				                    syncRepair(false), "rep_", n);
			}
			const DFT::Nodes::Inspection *insp = static_cast<const DFT::Nodes::Inspection *>(&node);
			if (insp->getPhases() == 0) {
				std::string l = "time " + insp->getLambda().str();
				EXPSyncItem *RR = new EXPSyncItem(l);
				unsigned int nodeID = nodeIDs[&node];

				EXPSyncRule rule(l, false);
				rule.syncOnNode = &node;
				rule.insertLabel(nodeID, RR);
				std::stringstream report("Added new timed sync rule: ");
				printSyncLineShort(report, rule);
				cc->reportAction2(report.str(),VERBOSITY_RULES);
				inspectionRules.push_back(rule);
			}
		} else if(DFT::Nodes::Node::typeMatch(node.getType(),
											  DFT::Nodes::ReplacementType))
		{
			addAnycastRule(repairRules, node, syncRepair(n + 1),
						   syncRepair(true), "rep_", n);
			addBroadcastRule(repairedRules, node, syncRepaired(n+1),
							 syncRepaired(0), "rpd_", n);
		}
	}

	/* Generate node-specific rules for the node */

	switch(node.getType()) {
		const DFT::Nodes::GateFDEP* fdep;
	case DFT::Nodes::GateType:
		cc->reportError("A gate should have a specialized type, not the general GateType");
		break;
	case DFT::Nodes::GatePhasedOrType:
		cc->reportErrorAt(node.getLocation(),"DFTreeEXPBuilder: unsupported gate: " + node.getTypeStr());
		break;
	case DFT::Nodes::GateHSPType:
		cc->reportErrorAt(node.getLocation(),"DFTreeEXPBuilder: unsupported gate: " + node.getTypeStr());
		break;
	case DFT::Nodes::GateCSPType:
		cc->reportErrorAt(node.getLocation(),"DFTreeEXPBuilder: unsupported gate: " + node.getTypeStr());
		break;
	case DFT::Nodes::GateFDEPType:
		fdep = static_cast<const DFT::Nodes::GateFDEP*>(&node);
		createSyncRuleGateFDEP(activationRules,failRules,*fdep,nodeID);
		break;
	case DFT::Nodes::GateTransferType:
		cc->reportErrorAt(node.getLocation(),"DFTreeEXPBuilder: unsupported gate: " + node.getTypeStr());
		break;
	default:
		break;
	}

	return 0;
}

void DFT::DFTreeEXPBuilder::calculateColumnWidths(
		vector<unsigned int>& columnWidths,
		const vector<DFT::EXPSyncRule>& syncRules)
{
	for (const DFT::EXPSyncRule &rule : syncRules) {
		std::map<unsigned int, std::shared_ptr<EXPSyncItem>>::const_iterator it = rule.label.begin();
		for(;it!=rule.label.end();++it) {
			assert( (0<=it->first) );
			assert( (it->first<columnWidths.size()) );
			std::string s = it->second->toStringQuoted();
			if(s.length() > columnWidths[it->first]) {
				columnWidths[it->first] = s.length();
			}
		}
	}
}
