"""Focused source-contract checks for `.botlearn start all`."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parent.parent
COMMANDS = ROOT / "src/game/Commands/Commands.cpp"
MANAGER_H = ROOT / "src/game/PlayerBots/PlayerBotMgr.h"
MANAGER_CPP = ROOT / "src/game/PlayerBots/PlayerBotMgr.cpp"
STORE = ROOT / "src/game/PlayerBots/Companion/LearningStore.h"


class BotLearnStartAllValueTest(unittest.TestCase):
    def test_command_accepts_case_insensitive_all_for_start_only(self):
        source = COMMANDS.read_text(encoding="utf-8")
        self.assertIn('if (targetKey == "all")', source)
        self.assertIn('if (action != "start")', source)
        self.assertIn("The 'all' target is only supported with start", source)
        self.assertIn("BotLearnStartAll(issuer, candidates)", source)

    def test_manager_counts_only_persistent_owned_companions(self):
        header = MANAGER_H.read_text(encoding="utf-8")
        source = MANAGER_CPP.read_text(encoding="utf-8")
        self.assertIn("bool BotLearnStartAll(Player* issuer, uint32& candidateCount);", header)
        body = re.search(
            r"bool PlayerBotMgr::BotLearnStartAll\(.*?\n\}(?=\n\nbool)",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(body)
        text = body.group(0)
        self.assertIn("issuer->GetSession()->GetAccountId()", text)
        self.assertIn("e->persistent && e->ownerAccountId == issuerAcc", text)
        self.assertIn("if (candidateCount == 0)", text)
        self.assertIn("m_learningStore.QueueStartAll(issuerAcc)", text)

    def test_store_uses_one_owner_scoped_roster_statement(self):
        source = STORE.read_text(encoding="utf-8")
        match = re.search(
            r'kControlStartAllSqlTemplate\s*=\s*"([^"]+)";', source
        )
        self.assertIsNotNone(match)
        sql = match.group(1)
        self.assertIn("INSERT INTO bot_learning_profile", sql)
        self.assertIn("FROM bot_ownership b JOIN playerbot p", sql)
        self.assertIn("b.owner_account_id = %u", sql)
        self.assertIn("ON DUPLICATE KEY UPDATE", sql)
        self.assertIn("IF(mode = 0", sql)
        self.assertEqual(sql.count("%u"), 2)

        method = re.search(
            r"bool QueueStartAll\(uint32_t ownerAccountId\).*?\n    \}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(method)
        text = method.group(0)
        self.assertIn("ControlMode::Observe", text)
        self.assertIn("EnqueueMaintenance(sql, false)", text)
        self.assertNotIn("QueueControl", text)

    def test_individual_command_path_remains_available(self):
        source = COMMANDS.read_text(encoding="utf-8")
        self.assertIn("sPlayerBotMgr.BotLearn(issuer, action, botName)", source)


if __name__ == "__main__":
    unittest.main()
