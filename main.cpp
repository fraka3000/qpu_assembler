/*
 * main.cpp
 *
 *  Created on: 15 Apr 2014
 *      Author: simon
 */

#include "InstructionTree.h"
#include "grammar.tab.hpp"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <map>
#include <set>
#include <vector>

int yyparse (void);

extern FILE *yyin;
extern std::list<Base *> s_statements;

std::list<Label *> s_declaredLabels;
std::list<Label *> s_usedLabels;
std::list<DependencyBase *> s_scoreboard;

void ClearScoreboard(void)
{
	s_scoreboard.clear();
}

std::list<Base *>::iterator BuildDeps(std::list<Base *>::iterator start)
{
	std::list<Base *>::iterator out = s_statements.end();

	ClearScoreboard();

	for (auto it = start; it != s_statements.end(); it++)
	{
		DependencyConsumer *pConsumer = dynamic_cast<DependencyConsumer *>(*it);
		DependencyProvider *pProvider = dynamic_cast<DependencyProvider *>(*it);

		if (pConsumer)
		{
			Dependee::Dependencies deps;
			pConsumer->GetInputDeps(deps);

			for (auto d = deps.begin(); d != deps.end(); d++)
			{
				for (auto s = s_scoreboard.begin(); s != s_scoreboard.end(); s++)
				{
					DependencyBase *p = *s;
					if ((*d)->SatisfiesThis(*p))
					{
						pConsumer->AddResolvedInputDep(*p);
						break;
					}
				}
			}
		}

		if (pProvider)
		{
			DependencyBase::Dependencies deps;
			pProvider->GetOutputDeps(deps);

			for (auto d = deps.begin(); d != deps.end(); d++)
			{
				for (auto s = s_scoreboard.begin(); s != s_scoreboard.end(); s++)
				{
					if ((*d)->ProvidesSameThing(*(*s)))
					{
						s_scoreboard.erase(s);
						break;
					}
				}

				s_scoreboard.push_back(*d);
			}
		}

		if (dynamic_cast<BranchInstruction *>(*it))
		{
			out = ++it;
			break;
		}
	}

	int count = 0;
	for (auto it = start; it != out; it++)
	{
		DependencyConsumer *pConsumer = dynamic_cast<DependencyConsumer *>(*it);
		if (pConsumer)
		{
			printf("instruction count %d, provider %p\n", count, dynamic_cast<DependencyProvider *>(*it));
			pConsumer->DebugPrintResolvedDeps();
			count++;
		}
	}

	return out;
}

Instruction &s_rSpareNop = AluInstruction::Nop();

void Schedule(std::list<DependencyProvider *> runInstructions, std::list<DependencyConsumer *> instructionsToRun, std::list<DependencyProvider *> &rBestSchedule, bool &rFoundSchedule, bool branchInserted, int delaySlotsToFill)
{
	assert(instructionsToRun.size() != 0);

	for (auto inst = instructionsToRun.begin(); inst != instructionsToRun.end(); inst++)
	{
		//check if this one can run
		DependencyBase::Dependencies deps = (*inst)->GetResolvedInputDeps();

		int nopsNeeded = 0;
		size_t canRun = 0;

		for (auto dep = deps.begin(); dep != deps.end(); dep++)
		{
			int nops;
			if (!(*dep)->CanRun(runInstructions, nops))
				break;
			else
			{
				if (nops > nopsNeeded)
					nopsNeeded = nops;
				canRun++;
			}
		}

		if (canRun != deps.size())
			break;

		std::list<DependencyProvider *> newRunInstructions = runInstructions;

		//we can run, and we know how many nops need to be inserted
		for (auto count = 0; count < nopsNeeded; count++)
			newRunInstructions.push_back(&s_rSpareNop);

		//add in the instruction
		Instruction *i = dynamic_cast<Instruction *>(*inst);
		assert(i);
		newRunInstructions.push_back(i);

		bool isBranch;
		if (dynamic_cast<BranchInstruction *>(i))
			isBranch = true;
		else
			isBranch = false;

		//only one branch
		assert((isBranch && !branchInserted)
				|| !isBranch);

		if (instructionsToRun.size() == 1)		//we already have processed the last one
		{
			//add delay slot nops
			for (auto count = 0; count < delaySlotsToFill; count++)
				newRunInstructions.push_back(&s_rSpareNop);

			if (rFoundSchedule)
			{
				if (rBestSchedule.size() < newRunInstructions.size())
					return;			//not worth it
				else
				{
					rBestSchedule = newRunInstructions;
					return;
				}
			}
			else
			{
				rBestSchedule = newRunInstructions;
				rFoundSchedule = true;
				return;
			}
		}
		else
		{
			//now make a new instructionsToRun
			std::list<DependencyConsumer *> newInstructionsToRun;
			for (auto it = instructionsToRun.begin(); it != instructionsToRun.end(); it++)
				if (*it != *inst)
					newInstructionsToRun.push_back(*it);

			//not worth pursuing
			if (rFoundSchedule && newInstructionsToRun.size() >= rBestSchedule.size())
				return;

			Schedule(newRunInstructions, newInstructionsToRun, rBestSchedule, rFoundSchedule,
					branchInserted ? branchInserted : isBranch,
					(branchInserted || isBranch) ? delaySlotsToFill - 1 : delaySlotsToFill);
		}
	}
}

int main(int argc, const char *argv[])
{
	unsigned int baseAddress = 0;

	if (argc >= 2)
		baseAddress = strtoll(argv[1], 0, 16);
	if (argc >= 3)
		yyin = fopen(argv[2], "r");

	printf("/""*\n");

	printf("base address is %08x\n", baseAddress);

	if (yyparse() != 0)
		assert(!"failure during yyparse\n");

	printf("initially read code\n");
	for (auto it = s_statements.begin(); it != s_statements.end(); it++)
		(*it)->DebugPrint(0);

	auto start = s_statements.begin();

	do
	{
		auto next_start = BuildDeps(start);
		std::list<DependencyProvider *> runInstructions;
		std::list<DependencyConsumer *> instructionsToRun;

		std::list<DependencyProvider *> bestSchedule;
		bool foundSchedule = false;

		for (auto it = start; it != next_start; it++)
		{
			DependencyConsumer *i = dynamic_cast<DependencyConsumer *>(*it);
			if (i)
				instructionsToRun.push_back(i);
		}

		if (instructionsToRun.size() != 0)
		{
			Schedule(runInstructions, instructionsToRun, bestSchedule, foundSchedule, false, 3);

			assert(foundSchedule);

			for (auto it = bestSchedule.begin(); it != bestSchedule.end(); it++)
				if (dynamic_cast<Base *>(*it))
					dynamic_cast<Base *>(*it)->DebugPrint(0);

			printf("sequence of %d instructions run in %d cycles\n", instructionsToRun.size(), bestSchedule.size());
		}

		start = next_start;
	} while (start != s_statements.end());
	exit(0);

	unsigned int address = baseAddress;

	//walk it once, to get addresses and fill in labels
	for (auto it = s_statements.begin(); it != s_statements.end(); it++)
	{
		Label *l = dynamic_cast<Label *>(*it);
		if (l)
			l->SetAddress(address);

		Assemblable *p = dynamic_cast<Assemblable *>(*it);
		if (p)
		{
			Assemblable::Fields f;
			p->Assemble(f);

			unsigned int sizeInBytes;
			uint64_t output;
			if (!Assemblable::CombineFields(f, sizeInBytes, output))
				assert(!"failed to combine fields\n");

			address += sizeInBytes;
		}
	}

	//link labels
	for (auto outer = s_usedLabels.begin(); outer != s_usedLabels.end(); outer++)
	{
		bool found = false;

		for (auto inner = s_declaredLabels.begin(); inner != s_declaredLabels.end(); inner++)
		{
			if (strcmp((*outer)->GetName(), (*inner)->GetName()) == 0)
			{
				(*outer)->Link(*inner);
				found = true;
				break;
			}
		}

		if (!found)
		{
			printf("undefined reference to \'%s\'\n", (*outer)->GetName());
			assert(0);
		}
	}

	printf("*""/\n");

	address = baseAddress;

	//and a second time for the output
	for (auto it = s_statements.begin(); it != s_statements.end(); it++)
	{
		Assemblable *p = dynamic_cast<Assemblable *>(*it);
		if (p)
		{
			Assemblable::Fields f;
			p->Assemble(f);

			unsigned int sizeInBytes;
			uint64_t output;
			if (!Assemblable::CombineFields(f, sizeInBytes, output))
				assert(!"failed to combine fields\n");

			printf("/*%08x*/\t", address);

			switch (sizeInBytes)
			{
			case 0:			//labels
				printf("\n");
				break;
			case 1:
				printf("0x%02llx,\n", output & 0xff);
				break;
			case 2:
				printf("0x%04llx,\n", output & 0xffff);
				break;
			case 4:
				printf("0x%08llx,\n", output & 0xffffffff);
				break;
			case 8:
				printf("0x%08llx, 0x%08llx,\n", output & 0xffffffff, output >> 32);
				break;
			default:
				assert(0);
			}

			address += sizeInBytes;
		}
	}

return 0;
}

