#include <algorithm>
#include <mutex>
#include <utility>
#include "Dungeon.h"
#include "Account.h"
#include "Assert.h"
#include "GameDB.h"
#include "ItemUID.h"

enum : int32_t
{
	CHECK_DIE = 20,       // 판정 주사위 d20
	MEMBER_DAMAGE_DIE = 6, // 파티원 공격 피해 d6 + 무기 피해
	TRAP_DAMAGE_DIE = 4,
	POTION_DIE = 6,       // 회복 d6 + POTION_BONUS
	POTION_BONUS = 2,
};

Dungeon::Dungeon(const GameData* gameData, GameDB* gameDB, std::vector<DungeonMember> members, const uint32_t lastTurnNo)
	: mGameData(gameData)
	, mGameDB(gameDB)
	, mMembers(std::move(members))
	, mTurnNo(lastTurnNo)
	, mRng(std::random_device{}())
{
	Assert(gameData != nullptr && !mMembers.empty() && mMembers.size() <= PARTY_MEMBER_MAX, "Dungeon: 데이터가 없거나 파티원 수가 범위 밖");
}

void Dungeon::Begin(std::vector<DungeonEventNotify>* outEvents)
{
	Assert(outEvents != nullptr, "Begin: outEvents가 null");
	enterRoom(0, outEvents);
}

eActResult Dungeon::Act(const int32_t memberIdx, const uint32_t turnNo, const eDungeonAction action, const int32_t target,
	std::vector<DungeonEventNotify>* outEvents)
{
	Assert(outEvents != nullptr, "Act: outEvents가 null");
	if (mbOver || memberIdx != mActorIdx || turnNo != mTurnNo)
	{
		return eActResult::Ignored; // 내 차례가 아니거나 이미 넘어간 턴의 늦은 요청
	}
	DungeonMember& member = mMembers[memberIdx];
	const RoomData& room = mGameData->GetRooms()[mRoomIdx];
	const uint8_t actor = static_cast<uint8_t>(memberIdx);

	switch (action)
	{
	case eDungeonAction::Attack:
	{
		if (target < 0 || target >= static_cast<int32_t>(mMonsters.size()) || mMonsters[target].Hp <= 0)
		{
			outEvents->push_back(makeEvent(eDungeonEvent::ActionRejected, actor)); // 밖에서 온 대상 번호 - 범위 밖·쓰러진 몬스터
			return eActResult::Rejected;
		}
		DungeonMonster& monster = mMonsters[target];
		DungeonEventNotify event = makeEvent(eDungeonEvent::Attack, actor);
		event.Target = static_cast<uint8_t>(ACTOR_MONSTER_FLAG | target);
		event.Roll = static_cast<uint8_t>(rollDie(CHECK_DIE));
		event.Bonus = static_cast<int8_t>(member.Attack);
		event.Difficulty = static_cast<uint8_t>(monster.Data->Defense);
		event.bSuccess = (event.Roll + member.Attack >= monster.Data->Defense) ? 1 : 0;
		if (event.bSuccess)
		{
			event.Damage = static_cast<int16_t>(rollDie(MEMBER_DAMAGE_DIE) + member.WeaponDamage);
			monster.Hp -= event.Damage;
		}
		outEvents->push_back(event);
		if (monster.Hp <= 0)
		{
			outEvents->push_back(makeEvent(eDungeonEvent::Down, static_cast<uint8_t>(ACTOR_MONSTER_FLAG | target)));
		}
		if (isAllMonstersDown())
		{
			if (room.Type == eRoomType::Boss)
			{
				finish(true);
			}
			else
			{
				enterRoom(mRoomIdx + 1, outEvents);
			}
			return eActResult::Done;
		}
		break;
	}
	case eDungeonAction::Defend:
		member.bDefending = true;
		outEvents->push_back(makeEvent(eDungeonEvent::Defend, actor));
		break;
	case eDungeonAction::Investigate:
	{
		if (room.Type != eRoomType::Investigate || member.bInvestigated)
		{
			outEvents->push_back(makeEvent(eDungeonEvent::ActionRejected, actor));
			return eActResult::Rejected;
		}
		member.bInvestigated = true;
		DungeonEventNotify event = makeEvent(eDungeonEvent::Investigate, actor);
		event.Roll = static_cast<uint8_t>(rollDie(CHECK_DIE));
		event.Bonus = static_cast<int8_t>(member.Investigate);
		event.Difficulty = static_cast<uint8_t>(room.Difficulty);
		event.bSuccess = (event.Roll + member.Investigate >= room.Difficulty) ? 1 : 0;
		if (event.bSuccess && room.Reward == eRoomReward::Potion)
		{
			std::lock_guard<std::mutex> playerLock(member.MemberAccount->GetLock()); // 파티 락 → 플레이어 락(가장 안쪽)
			if (!member.MemberAccount->IsInventoryFull())
			{
				const ItemInfo potion{ NewItemUID(), findPotionKind() };
				member.MemberAccount->AddItem(potion);
				mGameDB->SaveItemAdded(member.MemberAccount->GetID(), potion, "potion_found"); // 플레이어 락 안에서 - 메모리와 같은 순서로
			}
		}
		else if (event.bSuccess && room.Reward == eRoomReward::ExtraLoot)
		{
			mbExtraLoot = true;
		}
		outEvents->push_back(event);
		break;
	}
	case eDungeonAction::UsePotion:
	{
		if (member.Hp >= member.MaxHp)
		{
			outEvents->push_back(makeEvent(eDungeonEvent::ActionRejected, actor)); // 체력이 가득 차 있으면 회복약만 사라진다
			return eActResult::Rejected;
		}
		bool bHadPotion = false;
		{
			std::lock_guard<std::mutex> playerLock(member.MemberAccount->GetLock()); // 파티 락 → 플레이어 락(가장 안쪽)
			ItemInfo potion{};
			bHadPotion = member.MemberAccount->RemoveOneItemOfKind(findPotionKind(), &potion);
			if (bHadPotion)
			{
				mGameDB->SaveItemRemoved(member.MemberAccount->GetID(), potion, "potion_use");
			}
		}
		if (!bHadPotion)
		{
			outEvents->push_back(makeEvent(eDungeonEvent::ActionRejected, actor));
			return eActResult::Rejected;
		}
		const int32_t heal = std::min(rollDie(POTION_DIE) + POTION_BONUS, member.MaxHp - member.Hp);
		member.Hp += heal;
		DungeonEventNotify event = makeEvent(eDungeonEvent::UsePotion, actor);
		event.Damage = static_cast<int16_t>(heal);
		event.bSuccess = 1;
		outEvents->push_back(event);
		break;
	}
	default:
		outEvents->push_back(makeEvent(eDungeonEvent::ActionRejected, actor)); // 밖에서 온 모르는 행동
		return eActResult::Rejected;
	}
	advanceTurn(outEvents);
	return eActResult::Done;
}

void Dungeon::RemoveMember(const int32_t memberIdx, std::vector<DungeonEventNotify>* outEvents)
{
	Assert(outEvents != nullptr && memberIdx >= 0 && memberIdx < static_cast<int32_t>(mMembers.size()), "RemoveMember: outEvents가 null이거나 범위 밖 파티원 번호");
	mMembers.erase(mMembers.begin() + memberIdx);
	if (mbOver)
	{
		return;
	}
	if (mMembers.empty() || isAllMembersDown())
	{
		finish(false);
		return;
	}
	if (memberIdx < mActorIdx)
	{
		--mActorIdx; // 앞사람이 빠져 내 번호가 하나 당겨짐 - 차례는 그대로
	}
	else if (memberIdx == mActorIdx)
	{
		mActorIdx = memberIdx - 1; // 빠진 사람 다음부터 찾는다 - 라운드의 마지막이었으면 라운드가 끝난다
		advanceTurn(outEvents);
	}
}

bool Dungeon::AutoDefend(const uint32_t turnNo, std::vector<DungeonEventNotify>* outEvents)
{
	Assert(outEvents != nullptr, "AutoDefend: outEvents가 null");
	if (mbOver || turnNo != mTurnNo)
	{
		return false; // 이미 행동해 턴이 넘어갔다 - 옛 마감
	}
	mMembers[mActorIdx].bDefending = true;
	outEvents->push_back(makeEvent(eDungeonEvent::AutoDefend, static_cast<uint8_t>(mActorIdx)));
	advanceTurn(outEvents);
	return true;
}

bool Dungeon::IsOver() const
{
	return mbOver;
}

bool Dungeon::IsSuccess() const
{
	return mbSuccess;
}

bool Dungeon::HasExtraLoot() const
{
	return mbExtraLoot;
}

uint32_t Dungeon::GetTurnNo() const
{
	return mTurnNo;
}

void Dungeon::FillTurn(DungeonTurnNotify* outNotify) const
{
	Assert(outNotify != nullptr, "FillTurn: outNotify가 null");
	DungeonTurnNotify notify{};
	notify.Type = eGameType::DungeonTurnNotify;
	notify.TurnNo = mTurnNo;
	notify.RoomNo = static_cast<uint8_t>(mRoomIdx + 1);
	notify.ActorIdx = static_cast<uint8_t>(mActorIdx);
	notify.MemberCnt = static_cast<uint8_t>(mMembers.size());
	for (size_t i = 0; i < mMembers.size(); ++i)
	{
		notify.MemberHp[i] = static_cast<int16_t>(std::max(mMembers[i].Hp, 0));
	}
	notify.MonsterCnt = static_cast<uint8_t>(mMonsters.size());
	for (size_t i = 0; i < mMonsters.size(); ++i)
	{
		notify.MonsterKind[i] = mMonsters[i].Data->Kind;
		notify.MonsterHp[i] = static_cast<int16_t>(std::max(mMonsters[i].Hp, 0));
	}
	*outNotify = notify;
}

void Dungeon::enterRoom(const int32_t roomIdx, std::vector<DungeonEventNotify>* outEvents)
{
	const std::vector<RoomData>& rooms = mGameData->GetRooms();
	Assert(roomIdx < static_cast<int32_t>(rooms.size()), "enterRoom: 방 번호 범위 밖 - 마지막 방은 보스라 그 뒤로 가지 않는다");
	mRoomIdx = roomIdx;
	mRoundInRoom = 0;
	mMonsters.clear();
	for (DungeonMember& member : mMembers)
	{
		member.bInvestigated = false;
	}
	const RoomData& room = rooms[roomIdx];
	DungeonEventNotify enter = makeEvent(eDungeonEvent::EnterRoom, 0);
	outEvents->push_back(enter);

	if (room.Type == eRoomType::Trap)
	{
		// 들어가자마자 살아 있는 사람 전원 판정하고 바로 다음 방(설계 3-9 방 진행)
		for (size_t i = 0; i < mMembers.size(); ++i)
		{
			DungeonMember& member = mMembers[i];
			if (member.Hp <= 0)
			{
				continue;
			}
			DungeonEventNotify event = makeEvent(eDungeonEvent::Trap, static_cast<uint8_t>(i));
			event.Roll = static_cast<uint8_t>(rollDie(CHECK_DIE));
			event.Bonus = static_cast<int8_t>(member.Investigate);
			event.Difficulty = static_cast<uint8_t>(room.Difficulty);
			event.bSuccess = (event.Roll + member.Investigate >= room.Difficulty) ? 1 : 0;
			if (!event.bSuccess)
			{
				event.Damage = static_cast<int16_t>(rollDie(TRAP_DAMAGE_DIE));
			}
			outEvents->push_back(event);
			if (!event.bSuccess)
			{
				damageMember(static_cast<int32_t>(i), event.Damage, outEvents);
			}
		}
		if (isAllMembersDown())
		{
			finish(false);
			return;
		}
		enterRoom(roomIdx + 1, outEvents);
		return;
	}

	if (room.Type == eRoomType::Battle || room.Type == eRoomType::Boss)
	{
		const MonsterData* monster = mGameData->FindMonster(room.MonsterKind);
		Assert(monster != nullptr, "enterRoom: 데이터 확인을 통과한 방의 몬스터가 없음");
		const int32_t memberCnt = static_cast<int32_t>(mMembers.size());
		const int32_t monsterCnt = std::min(room.bMonsterPerMember ? memberCnt : room.MonsterCount, static_cast<int32_t>(DUNGEON_MONSTER_MAX));
		for (int32_t i = 0; i < monsterCnt; ++i)
		{
			mMonsters.push_back(DungeonMonster{ monster, monster->HpBase + monster->HpPerMember * memberCnt });
		}
	}
	startTurn(findAliveMemberFrom(0));
}

void Dungeon::startTurn(const int32_t actorIdx)
{
	Assert(actorIdx >= 0, "startTurn: 살아 있는 파티원이 없는데 차례를 정함");
	mActorIdx = actorIdx;
	++mTurnNo;
	mMembers[actorIdx].bDefending = false; // 지난 라운드의 방어는 내 차례가 오면 끝난다
}

void Dungeon::advanceTurn(std::vector<DungeonEventNotify>* outEvents)
{
	if (mbOver)
	{
		return;
	}
	const int32_t next = findAliveMemberFrom(mActorIdx + 1);
	if (next >= 0)
	{
		startTurn(next);
		return;
	}
	endRound(outEvents);
}

void Dungeon::endRound(std::vector<DungeonEventNotify>* outEvents)
{
	const RoomData& room = mGameData->GetRooms()[mRoomIdx];
	if (room.Type == eRoomType::Investigate)
	{
		enterRoom(mRoomIdx + 1, outEvents); // 조사 방은 한 라운드 뒤 다음 방
		return;
	}
	runMonsters(outEvents);
	++mRoundInRoom;
	if (isAllMembersDown())
	{
		finish(false);
		return;
	}
	startTurn(findAliveMemberFrom(0));
}

void Dungeon::runMonsters(std::vector<DungeonEventNotify>* outEvents)
{
	for (size_t m = 0; m < mMonsters.size(); ++m)
	{
		const DungeonMonster& monster = mMonsters[m];
		if (monster.Hp <= 0 || isAllMembersDown())
		{
			continue;
		}
		std::vector<int32_t> alive;
		for (size_t i = 0; i < mMembers.size(); ++i)
		{
			if (mMembers[i].Hp > 0)
			{
				alive.push_back(static_cast<int32_t>(i));
			}
		}
		const int32_t target = alive[static_cast<size_t>(rollDie(static_cast<int32_t>(alive.size())) - 1)]; // 살아 있는 사람 중 무작위
		const DungeonMember& member = mMembers[target];
		DungeonEventNotify event = makeEvent(eDungeonEvent::Attack, static_cast<uint8_t>(ACTOR_MONSTER_FLAG | m));
		event.Target = static_cast<uint8_t>(target);
		event.Roll = static_cast<uint8_t>(rollDie(CHECK_DIE));
		event.Bonus = static_cast<int8_t>(monster.Data->AttackBonus);
		event.Difficulty = static_cast<uint8_t>(member.Defense);
		event.bSuccess = (event.Roll + monster.Data->AttackBonus >= member.Defense) ? 1 : 0;
		if (event.bSuccess)
		{
			const int32_t damage = rollDie(monster.Data->DamageDie);
			event.Damage = static_cast<int16_t>(member.bDefending ? damage / 2 : damage);
		}
		outEvents->push_back(event);
		if (event.bSuccess)
		{
			damageMember(target, event.Damage, outEvents);
		}

		// 보스 특수 공격 - N라운드마다 살아 있는 파티원 전원
		if (monster.Data->SpecialEvery > 0 && (mRoundInRoom + 1) % monster.Data->SpecialEvery == 0)
		{
			for (size_t i = 0; i < mMembers.size(); ++i)
			{
				if (mMembers[i].Hp <= 0)
				{
					continue;
				}
				const int32_t damage = rollDie(monster.Data->SpecialDie);
				DungeonEventNotify special = makeEvent(eDungeonEvent::Special, static_cast<uint8_t>(ACTOR_MONSTER_FLAG | m));
				special.Target = static_cast<uint8_t>(i);
				special.bSuccess = 1;
				special.Damage = static_cast<int16_t>(mMembers[i].bDefending ? damage / 2 : damage);
				outEvents->push_back(special);
				damageMember(static_cast<int32_t>(i), special.Damage, outEvents);
			}
		}
	}
}

void Dungeon::damageMember(const int32_t memberIdx, const int32_t damage, std::vector<DungeonEventNotify>* outEvents)
{
	DungeonMember& member = mMembers[memberIdx];
	const bool bWasAlive = member.Hp > 0;
	member.Hp -= damage;
	if (bWasAlive && member.Hp <= 0)
	{
		outEvents->push_back(makeEvent(eDungeonEvent::Down, static_cast<uint8_t>(memberIdx)));
	}
}

void Dungeon::finish(const bool bSuccess)
{
	mbOver = true;
	mbSuccess = bSuccess;
}

bool Dungeon::isAllMembersDown() const
{
	return findAliveMemberFrom(0) < 0;
}

bool Dungeon::isAllMonstersDown() const
{
	for (const DungeonMonster& monster : mMonsters)
	{
		if (monster.Hp > 0)
		{
			return false;
		}
	}
	return true;
}

int32_t Dungeon::findAliveMemberFrom(const int32_t startIdx) const
{
	for (int32_t i = std::max(startIdx, 0); i < static_cast<int32_t>(mMembers.size()); ++i)
	{
		if (mMembers[i].Hp > 0)
		{
			return i;
		}
	}
	return -1;
}

int32_t Dungeon::rollDie(const int32_t sides)
{
	std::uniform_int_distribution<int32_t> die(1, sides);
	return die(mRng);
}

DungeonEventNotify Dungeon::makeEvent(const eDungeonEvent event, const uint8_t actor) const
{
	DungeonEventNotify notify{};
	notify.Type = eGameType::DungeonEventNotify;
	notify.Event = event;
	notify.Actor = actor;
	notify.RoomNo = static_cast<uint8_t>(mRoomIdx + 1);
	return notify;
}

std::vector<std::vector<uint16_t>> Dungeon::RollLoot()
{
	Assert(mbOver && mbSuccess, "RollLoot: 보스를 잡지 않았는데 전리품을 굴림");
	std::vector<std::vector<uint16_t>> loot(mMembers.size());
	const int32_t lootCnt = static_cast<int32_t>(mMembers.size()) + (mbExtraLoot ? 1 : 0);
	for (int32_t i = 0; i < lootCnt; ++i)
	{
		const uint16_t kind = rollLootKind();
		if (kind != 0)
		{
			loot[static_cast<size_t>(rollLootWinner())].push_back(kind); // 쓰러진 사람도 분배에는 참여(시나리오 4장)
		}
	}
	return loot;
}

uint16_t Dungeon::rollLootKind()
{
	int32_t roll = rollDie(100);
	int32_t gradeIdx = 0;
	for (; gradeIdx < static_cast<int32_t>(eItemGrade::Count) - 1; ++gradeIdx)
	{
		roll -= mGameData->GetGradeChance(static_cast<eItemGrade>(gradeIdx));
		if (roll <= 0)
		{
			break;
		}
	}
	const eItemGrade grade = static_cast<eItemGrade>(gradeIdx);
	std::vector<uint16_t> candidates;
	for (const ItemData& item : mGameData->GetItems())
	{
		if (item.bEquippable && item.Grade == grade)
		{
			candidates.push_back(item.Kind); // 전리품은 장비만 - 회복약은 조사 보상으로만 나온다
		}
	}
	if (candidates.empty())
	{
		return 0;
	}
	return candidates[static_cast<size_t>(rollDie(static_cast<int32_t>(candidates.size())) - 1)];
}

int32_t Dungeon::rollLootWinner()
{
	std::vector<int32_t> contenders;
	for (size_t i = 0; i < mMembers.size(); ++i)
	{
		contenders.push_back(static_cast<int32_t>(i));
	}
	while (contenders.size() > 1)
	{
		std::vector<int32_t> best;
		int32_t bestRoll = 0;
		for (const int32_t memberIdx : contenders)
		{
			const int32_t roll = rollDie(CHECK_DIE);
			if (roll > bestRoll)
			{
				bestRoll = roll;
				best.clear();
			}
			if (roll == bestRoll)
			{
				best.push_back(memberIdx);
			}
		}
		contenders = best; // 같으면 그 사람들끼리 다시
	}
	return contenders[0];
}

uint16_t Dungeon::findPotionKind() const
{
	return mGameData->GetPotionKind(); // 데이터 확인이 하나는 있다고 보장한다
}
