/********************************************************************** 
 Freeciv - Copyright (C) 2005 The Freeciv Team
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>

#include "fcintl.h"
#include "log.h"
#include "mem.h"
#include "rand.h"
#include "shared.h"
#include "support.h"

#include "game.h"
#include "government.h"
#include "movement.h"
#include "player.h"
#include "tech.h"
#include "unit.h"

#include "script.h"

#include "citytools.h"
#include "cityturn.h"
#include "gamehand.h"
#include "maphand.h"
#include "plrhand.h"
#include "techtools.h"
#include "unittools.h"

static Tech_type_id pick_random_tech(struct player* plr);

/**************************************************************************
...
**************************************************************************/
void do_dipl_cost(struct player *pplayer, Tech_type_id tech)
{
  struct player_research * research = get_player_research(pplayer);

  research->bulbs_researched
    -= (base_total_bulbs_required(pplayer, tech) * game.info.diplcost) / 100;
  research->changed_from = -1;
}

/**************************************************************************
...
**************************************************************************/
void do_free_cost(struct player *pplayer, Tech_type_id tech)
{
  struct player_research * research = get_player_research(pplayer);

  research->bulbs_researched
    -= (base_total_bulbs_required(pplayer, tech) * game.info.freecost) / 100;
  research->changed_from = -1;
}

/**************************************************************************
...
**************************************************************************/
void do_conquer_cost(struct player *pplayer, Tech_type_id tech)
{
  struct player_research * research = get_player_research(pplayer);  

  research->bulbs_researched
    -= (base_total_bulbs_required(pplayer, tech) * game.info.conquercost) / 100;
  research->changed_from = -1;
}

/****************************************************************************
  Called to find and choose (pick) a research target on the way to the
  player's goal.  Return a tech iff the tech is set.
****************************************************************************/
Tech_type_id choose_goal_tech(struct player *plr)
{
  Tech_type_id sub_goal;
  struct player_research *research = get_player_research(plr);

  sub_goal = get_next_tech(plr, research->tech_goal);

  if (sub_goal != A_UNSET) {
    choose_tech(plr, sub_goal);
  }
  return sub_goal;
}

/****************************************************************************
  Player has researched a new technology
****************************************************************************/
static void tech_researched(struct player *plr)
{
  struct player_research *research = get_player_research(plr);
  /* plr will be notified when new tech is chosen */
  Tech_type_id tech_id = research->researching;
  struct advance *tech = &advances[tech_id];

  if (!is_future_tech(research->researching)) {
    notify_embassies(plr, NULL, NULL, E_TECH_GAIN,
		     _("The %s have researched %s."), 
		     get_nation_name_plural(plr->nation),
		     get_tech_name(plr, research->researching));

  } else {
    notify_embassies(plr, NULL, NULL, E_TECH_GAIN,
		     _("The %s have researched Future Tech. %d."), 
		     get_nation_name_plural(plr->nation),
		     research->future_tech);
  
  }
  script_signal_emit("tech_researched", 3,
		     API_TYPE_TECH_TYPE, tech, API_TYPE_PLAYER, plr,
		     API_TYPE_STRING, "researched");

  /* Deduct tech cost */
  research->bulbs_researched = 
      MAX(research->bulbs_researched - total_bulbs_required(plr), 0);

  /* do all the updates needed after finding new tech */
  found_new_tech(plr, research->researching, TRUE, TRUE);
}

/****************************************************************************
  Give technologies to players with EFT_TECH_PARASITE (traditionally from
  the Great Library).
****************************************************************************/
void do_tech_parasite_effect(struct player *pplayer)
{
  int mod;
  struct effect_list *plist = effect_list_new();

  /* Note that two EFT_TECH_PARASITE effects will combine into a single,
   * much worse effect. */
  if ((mod = get_player_bonus_effects(plist, pplayer,
				      EFT_TECH_PARASITE)) > 0) {
    char buf[512];

    buf[0] = '\0';
    effect_list_iterate(plist, peffect) {
      if (buf[0] != '\0') {
	sz_strlcat(buf, ", ");
      }
      get_effect_req_text(peffect, buf, sizeof(buf));
    } effect_list_iterate_end;

    tech_type_iterate(i) {
      if (get_invention(pplayer, i) != TECH_KNOWN
	  && tech_is_available(pplayer, i)) {
	int num_players = 0;

	players_iterate(aplayer) {
	  if (get_invention(aplayer, i) == TECH_KNOWN) {
	    num_players++;
	  }
	} players_iterate_end;
	if (num_players >= mod) {
	  notify_player(pplayer, NULL, E_TECH_GAIN,
			   _("%s acquired from %s!"),
			   get_tech_name(pplayer, i), buf);
	  script_signal_emit("tech_researched", 3,
			     API_TYPE_TECH_TYPE, &advances[i],
			     API_TYPE_PLAYER, pplayer,
			     API_TYPE_STRING, "stolen");
	  notify_embassies(pplayer, NULL, NULL, E_TECH_GAIN,
			   _("The %s have acquired %s from %s."),
			   get_nation_name_plural(pplayer->nation),
			   get_tech_name(pplayer, i), buf);

	  do_free_cost(pplayer, i);
	  found_new_tech(pplayer, i, FALSE, TRUE);
	  break;
	}
      }
    } tech_type_iterate_end;
  }
  effect_list_unlink_all(plist);
  effect_list_free(plist);
}

/****************************************************************************
  Update all player specific stuff after the tech_found was discovered.
  could_switch_to_government holds information about which 
  government the player could switch to before the tech was reached
****************************************************************************/
static void update_player_after_tech_researched(struct player* plr,
                                         Tech_type_id tech_found,
					 bool was_discovery,
					 bool* could_switch_to_government)
{
  update_research(plr);

  remove_obsolete_buildings(plr);
  
  /* Give free rails in every city */
  if (tech_flag(tech_found, TF_RAILROAD)) {
    upgrade_city_rails(plr, was_discovery);  
  }
  
  /* Enhance vision of units if a player-ranged effect has changed.  Note
   * that world-ranged effects will not be updated immediately. */
  unit_list_refresh_vision(plr->units);

  /* Notify a player about new governments available */
  government_iterate(gov) {
    if (!could_switch_to_government[gov->index]
	&& can_change_to_government(plr, gov)) {
      notify_player(plr, NULL, E_NEW_GOVERNMENT,
		       _("Discovery of %s makes the government form %s"
			 " available. You may want to start a revolution."),
		       get_tech_name(plr, tech_found), gov->name);
    }
  } government_iterate_end;

  /*
   * Inform player about his new tech.
   */
  send_player_info(plr, plr);
}

/****************************************************************************
  Fill the array which contains information about which government
  the player can switch to.
****************************************************************************/
static void fill_can_switch_to_government_array(struct player* plr, bool* can_switch)
{
  government_iterate(gov) {
    /* We do it this way so all requirements are checked, including
     * statue-of-liberty effects. */
    can_switch[gov->index] = can_change_to_government(plr, gov);
  } government_iterate_end;
} 

/****************************************************************************
  Fill the array which contains information about value of the
  EFT_HAVE_EMBASSIES effect for each player
****************************************************************************/
static void fill_have_embassies_array(int* have_embassies)
{
  players_iterate(aplr) {
    have_embassies[aplr->player_no]
      = get_player_bonus(aplr, EFT_HAVE_EMBASSIES);
  } players_iterate_end;
}

/****************************************************************************
  Player has a new technology (from somewhere). was_discovery is passed 
  on to upgrade_city_rails. Logging & notification is not done here as 
  it depends on how the tech came. If next_tech is other than A_NONE, this 
  is the next tech to research.
****************************************************************************/
void found_new_tech(struct player *plr, Tech_type_id tech_found,
		    bool was_discovery, bool saving_bulbs)
{
  bool bonus_tech_hack = FALSE;
  bool was_first = FALSE;
  int had_embassies[MAX_NUM_PLAYERS + MAX_NUM_BARBARIANS];
  struct city *pcity;
  bool can_switch[MAX_NUM_PLAYERS + MAX_NUM_BARBARIANS]
                 [game.control.government_count];
  struct player_research *research = get_player_research(plr);

  /* HACK: A_FUTURE doesn't "exist" and is thus not "available".  This may
   * or may not be the correct thing to do.  For these sanity checks we
   * just special-case it. */
  assert((tech_exists(tech_found)
	  && get_invention(plr, tech_found) != TECH_KNOWN)
	 || tech_found == A_FUTURE);
  assert(tech_is_available(plr, tech_found) || tech_found == A_FUTURE);

  /* got_tech allows us to change research without applying techpenalty
   * (without loosing bulbs) */
  if (tech_found == research->researching) {
    research->got_tech = TRUE;
  }
  research->changed_from = -1;
  research->techs_researched++;
  was_first = (!game.info.global_advances[tech_found]);

  if (was_first) {
    /* Alert the owners of any wonders that have been made obsolete */
    impr_type_iterate(id) {
      if (is_great_wonder(id) && great_wonder_was_built(id)
	  && get_improvement_type(id)->obsolete_by == tech_found
	  && (pcity = find_city_from_great_wonder(id))) {
	notify_player(city_owner(pcity), NULL, E_WONDER_OBSOLETE,
	                 _("Discovery of %s OBSOLETES %s in %s!"), 
	                 get_tech_name(city_owner(pcity), tech_found),
			 get_improvement_name(id),
	                 pcity->name);
      }
    } impr_type_iterate_end;
  }

  if (tech_flag(tech_found, TF_BONUS_TECH) && was_first) {
    bonus_tech_hack = TRUE;
  }
  
  /* Count EFT_HAVE_EMBASSIES effect for each player.
   * We will check what has changed later */
  fill_have_embassies_array(had_embassies);

  /* Memorize some values before the tech is marked as researched.
   * They will be used to notify a player about a change */
  players_iterate(aplayer) {
    if (!players_on_same_team(aplayer, plr)) {
      continue;
    }
    fill_can_switch_to_government_array(aplayer,
                                        can_switch[aplayer->player_no]);
  } players_iterate_end;


  /* Mark the tech as known in the research struct and update
   * global_advances array */
  set_invention(plr, tech_found, TECH_KNOWN);

  /* Make proper changes for all players sharing the research */  
  players_iterate(aplayer) {
    if (!players_on_same_team(aplayer, plr)) {
      continue;
    }
    update_player_after_tech_researched(aplayer, tech_found, was_discovery,
                                        can_switch[aplayer->player_no]);
  } players_iterate_end;

  if (tech_found == research->tech_goal) {
    research->tech_goal = A_UNSET;
  }

  if (tech_found == research->researching) {
    Tech_type_id next_tech = choose_goal_tech(plr);
    /* try to pick new tech to research */

    if (next_tech != A_UNSET) {
      notify_team(plr, NULL, E_TECH_LEARNED,
		       _("Learned %s. "
			 "Our scientists focus on %s; goal is %s."),
		       get_tech_name(plr, tech_found),
		       get_tech_name(plr, research->researching),
		       get_tech_name(plr, research->tech_goal));
    } else {
      if (plr->ai.control) {
        choose_random_tech(plr);
      } else if (is_future_tech(tech_found)) {
        /* Continue researching future tech. */
        research->researching = A_FUTURE;
      } else {
        research->researching = A_UNSET;
      }
      if (research->researching != A_UNSET 
          && (!is_future_tech(research->researching)
	      || !is_future_tech(tech_found))) {
	notify_team(plr, NULL, E_TECH_LEARNED,
			 _("Learned %s.  Scientists "
			   "choose to research %s."),
			 get_tech_name(plr, tech_found),
			 get_tech_name(plr, research->researching));
      } else if (research->researching != A_UNSET) {
	char buffer1[300], buffer2[300];

	my_snprintf(buffer1, sizeof(buffer1), _("Learned %s. "),
		    get_tech_name(plr, research->researching));
	research->future_tech++;
	my_snprintf(buffer2, sizeof(buffer2), _("Researching %s."),
		    get_tech_name(plr, research->researching));
	notify_team(plr, NULL, E_TECH_LEARNED, "%s%s", buffer1,
			 buffer2);
      } else {
	notify_team(plr, NULL, E_TECH_LEARNED,
			 _("Learned %s.  Scientists "
			   "do not know what to research next."),
			 get_tech_name(plr, tech_found));
      }
    }
  }

  if (!saving_bulbs && research->bulbs_researched > 0) {
    research->bulbs_researched = 0;
  }

  if (bonus_tech_hack) {
    if (advances[tech_found].bonus_message) {
      notify_team(plr, NULL, E_TECH_GAIN,
		     "%s", _(advances[tech_found].bonus_message));
    } else {
      notify_team(plr, NULL, E_TECH_GAIN,
                     _("Great scientists from all the "
		       "world join your civilization: you get "
		       "an immediate advance."));
    }
    
    if (research->researching == A_UNSET) {
      choose_random_tech(plr);
    }
    tech_researched(plr);
  }

  /*
   * Inform all players about new global advances to give them a
   * chance to obsolete buildings.
   */
  send_game_info(NULL);

  /*
   * Update all cities in case the tech changed some effects. This is
   * inefficient; it could be optimized if it's found to be a problem.  But
   * techs aren't researched that often.
   */
  cities_iterate(pcity) {
    city_refresh(pcity);
    send_city_info(city_owner(pcity), pcity);
  } cities_iterate_end;
  
  /*
   * Send all player an updated info of the owner of the Marco Polo
   * Wonder if this wonder has become obsolete.
   */
  players_iterate(owner) {
    if (had_embassies[owner->player_no]  > 0
	&& get_player_bonus(owner, EFT_HAVE_EMBASSIES) == 0) {
      players_iterate(other_player) {
	send_player_info(owner, other_player);
      } players_iterate_end;
    }
  } players_iterate_end;
}

/****************************************************************************
  Adds the given number of  bulbs into the player's tech and 
  (if necessary) completes the research.
  The caller is responsible for sending updated player information.
  This is called from each city every turn and from caravan revenue
****************************************************************************/
void update_tech(struct player *plr, int bulbs)
{
  int excessive_bulbs;
  struct player_research *research = get_player_research(plr);

  /* count our research contribution this turn */
  plr->bulbs_last_turn += bulbs;

  research->bulbs_researched += bulbs;

  if (research->researching != A_UNSET) {  
    excessive_bulbs =
      (research->bulbs_researched - total_bulbs_required(plr));

    if (excessive_bulbs >= 0) {
      tech_researched(plr);
      if (research->researching != A_UNSET) {
        update_tech(plr, 0);
      }
    }
  }
}

/****************************************************************************
  Returns random researchable tech or A_FUTURE.
  No side effects
****************************************************************************/
static Tech_type_id pick_random_tech(struct player* plr) 
{
  int chosen, researchable = 0;

  tech_type_iterate(i) {
    if (get_invention(plr, i) == TECH_REACHABLE) {
      researchable++;
    }
  } tech_type_iterate_end;
  if (researchable == 0) {
    return A_FUTURE;
  }
  chosen = myrand(researchable) + 1;
  
  tech_type_iterate(i) {
    if (get_invention(plr, i) == TECH_REACHABLE) {
      chosen--;
      if (chosen == 0) {
        return i;
      }
    }
  } tech_type_iterate_end;
  assert(0);
  return A_FUTURE;
}

/****************************************************************************
  Finds and chooses (sets) a random research target from among all those
  available until plr->research->researching != A_UNSET.
  Player may research more than one tech in this function.
  Possible reasons:
  - techpenalty < 100
  - research.got_tech = TRUE and enough bulbs was saved
  - research.researching = A_UNSET and enough bulbs was saved
****************************************************************************/
void choose_random_tech(struct player *plr)
{
  struct player_research* research = get_player_research(plr);
  do {
    choose_tech(plr, pick_random_tech(plr));
  } while (research->researching == A_UNSET);
}

/****************************************************************************
  Called when the player chooses the tech he wants to research (or when
  the server chooses it for him automatically).

  This takes care of all side effects so the player's research target
  probably shouldn't be changed outside of this function (doing so has
  been the cause of several bugs).
****************************************************************************/
void choose_tech(struct player *plr, Tech_type_id tech)
{
  struct player_research *research = get_player_research(plr);

  if (research->researching == tech) {
    return;
  }
  if (get_invention(plr, tech) != TECH_REACHABLE) {
    /* can't research this */
    return;
  }
  if (!research->got_tech && research->changed_from == -1) {
    research->bulbs_researched_before = research->bulbs_researched;
    research->changed_from = research->researching;
    /* subtract a penalty because we changed subject */
    if (research->bulbs_researched > 0) {
      research->bulbs_researched
	-= ((research->bulbs_researched * game.info.techpenalty) / 100);
      assert(research->bulbs_researched >= 0);
    }
  } else if (tech == research->changed_from) {
    research->bulbs_researched = research->bulbs_researched_before;
    research->changed_from = -1;
  }
  research->researching=tech;
  if (research->bulbs_researched > total_bulbs_required(plr)) {
    tech_researched(plr);
  }
}

/****************************************************************************
  Called when the player chooses the tech goal he wants to research (or when
  the server chooses it for him automatically).
****************************************************************************/
void choose_tech_goal(struct player *plr, Tech_type_id tech)
{
  struct player_research *research = get_player_research(plr);

  if (research && tech != research->tech_goal) {
    /* It's been suggested that if the research target is empty then
     * choose_random_tech should be called here. */
    research->tech_goal = tech;
    notify_research(plr, E_TECH_GOAL,
		    _("Technology goal is %s."),
		    get_tech_name(plr, tech));
  }
}

/****************************************************************************
  Initializes tech data for the player.
****************************************************************************/
void init_tech(struct player *plr)
{
  tech_type_iterate(i) {
    set_invention(plr, i, TECH_UNKNOWN);
  } tech_type_iterate_end;
  set_invention(plr, A_NONE, TECH_KNOWN);

  get_player_research(plr)->techs_researched = 1;

  /* Mark the reachable techs */
  update_research(plr);
  if (choose_goal_tech(plr) == A_UNSET) {
    choose_random_tech(plr);
  }
}

/****************************************************************************
  Gives initial techs to the player
****************************************************************************/
void give_initial_techs(struct player* plr)
{
  struct nation_type *nation = get_nation_by_plr(plr);
  int i;
  
  /*
   * Give game wide initial techs
   */
  for (i = 0; i < MAX_NUM_TECH_LIST; i++) {
    if (game.rgame.global_init_techs[i] == A_LAST) {
      break;
    }
    found_new_tech(plr, game.rgame.global_init_techs[i], FALSE, TRUE);
  }

  /*
   * Give nation specific initial techs
   */
  for (i = 0; i < MAX_NUM_TECH_LIST; i++) {
    if (nation->init_techs[i] == A_LAST) {
      break;
    }
    found_new_tech(plr, nation->init_techs[i], FALSE, TRUE);
  }
}

/****************************************************************************
  Gives a player random tech, which he hasn't researched yet.
  Returns the tech. This differs from give_random_free_tech - it doesn't
  apply free cost
****************************************************************************/
Tech_type_id give_random_initial_tech(struct player* pplayer)
{
  Tech_type_id tech;
  
  tech = pick_random_tech(pplayer);
  found_new_tech(pplayer, tech, FALSE, TRUE);
  return tech;
}

/****************************************************************************
  If victim has a tech which pplayer doesn't have, pplayer will get it.
  The clients will both be notified and the conquer cost
  penalty applied. Used for diplomats and city conquest.
  If preferred is A_UNSET one random tech will be choosen.
  Returns the stolen tech or A_NONE if no tech was found.
****************************************************************************/
Tech_type_id steal_a_tech(struct player *pplayer, struct player *victim,
            	        Tech_type_id preferred)
{
  Tech_type_id stolen_tech = A_NONE;
  
  if (preferred == A_UNSET) {
    int j = 0;
    tech_type_iterate(i) {
      if (get_invention(pplayer, i) != TECH_KNOWN
	  && get_invention(victim, i) == TECH_KNOWN
	  && tech_is_available(pplayer, i)) {
        j++;
      }
    } tech_type_iterate_end;
  
    if (j == 0)  {
      /* we've moved on to future tech */
      if (get_player_research(victim)->future_tech
        > get_player_research(pplayer)->future_tech) {
        found_new_tech(pplayer, A_FUTURE, FALSE, TRUE);	
        stolen_tech = A_FUTURE;
      } else {
        return A_NONE;
      }
    } else {
      /* pick random tech */
      j = myrand(j) + 1;
      stolen_tech = A_NONE; /* avoid compiler warning */
      tech_type_iterate(i) {
        if (get_invention(pplayer, i) != TECH_KNOWN
	    && get_invention(victim, i) == TECH_KNOWN
	    && tech_is_available(pplayer, i)) {
	  j--;
        }
        if (j == 0) {
	  stolen_tech = i;
	  break;
        }
      } tech_type_iterate_end;
      assert(stolen_tech != A_NONE);
    }
  } else { /* preferred != A_UNSET */
    assert((preferred == A_FUTURE
            && get_invention(victim, A_FUTURE) == TECH_REACHABLE)
	   || (tech_exists(preferred)
	       && get_invention(victim, preferred) == TECH_KNOWN));
    stolen_tech = preferred;
  }
  script_signal_emit("tech_researched", 3,
		     API_TYPE_TECH_TYPE, &advances[stolen_tech],
		     API_TYPE_PLAYER, pplayer,
		     API_TYPE_STRING, "stolen");

  notify_player(pplayer, NULL, E_TECH_GAIN,
		   _("You steal %s from the %s."),
		   get_tech_name(pplayer, stolen_tech),
		   get_nation_name_plural(victim->nation));

  notify_player(victim, NULL, E_ENEMY_DIPLOMAT_THEFT,
                   _("The %s stole %s from you!"),
		   get_nation_name_plural(pplayer->nation),
		   get_tech_name(pplayer, stolen_tech));

  notify_embassies(pplayer, victim, NULL, E_TECH_GAIN,
		   _("The %s have stolen %s from the %s."),
		   get_nation_name_plural(pplayer->nation),
		   get_tech_name(pplayer, stolen_tech),
		   get_nation_name_plural(victim->nation));

  do_conquer_cost(pplayer, stolen_tech);
  found_new_tech(pplayer, stolen_tech, FALSE, TRUE);
  return stolen_tech;
}

/****************************************************************************
  Handle incoming player_research packet. Need to check correctness
  Set the player to be researching the given tech.

  If there are enough accumulated research points, the tech may be
  acquired immediately.
****************************************************************************/
void handle_player_research(struct player *pplayer, int tech)
{
  if (tech != A_FUTURE && !tech_exists(tech)) {
    return;
  }
  
  if (tech != A_FUTURE && get_invention(pplayer, tech) != TECH_REACHABLE) {
    return;
  }
  
  choose_tech(pplayer, tech);
  send_player_info(pplayer, pplayer);

  /* Notify Team members. 
   * They share same research struct.
   */
  players_iterate(aplayer) {
    if (pplayer != aplayer
	&& pplayer->diplstates[aplayer->player_no].type == DS_TEAM
	&& aplayer->is_alive) {
      send_player_info(aplayer, aplayer);
    }
  } players_iterate_end;
}

/****************************************************************************
  Handle incoming player_tech_goal packet
  Called from the network or AI code to set the player's tech goal.
****************************************************************************/
void handle_player_tech_goal(struct player *pplayer, int tech_goal)
{
  if (tech_goal != A_FUTURE && !tech_exists(tech_goal)) {
    return;
  }
  
  if (tech_goal != A_FUTURE && !tech_is_available(pplayer, tech_goal)) {
    return;
  }
  
  if (tech_goal == A_NONE) {
    /* A_NONE "exists" but is not allowed as a tech goal.  A_UNSET should
     * be used instead.  However the above checks may prevent the client from
     * ever setting the goal to A_UNSET, meaning once a goal is set it
     * can't be removed. */
    return;
  }

  choose_tech_goal(pplayer, tech_goal);
  send_player_info(pplayer, pplayer);

  /* Notify Team members */
  players_iterate(aplayer) {
    if (pplayer != aplayer
        && pplayer->diplstates[aplayer->player_no].type == DS_TEAM
        && aplayer->is_alive
        && get_player_research(aplayer)->tech_goal != tech_goal) {
      handle_player_tech_goal(aplayer, tech_goal);
    }
  } players_iterate_end;
}

/****************************************************************************
  Gives a player random tech, which he hasn't researched yet. Applies freecost
  Returns the tech.
****************************************************************************/
Tech_type_id give_random_free_tech(struct player* pplayer)
{
  Tech_type_id tech;
  
  tech = pick_random_tech(pplayer);
  do_free_cost(pplayer, tech);
  found_new_tech(pplayer, tech, FALSE, TRUE);
  return tech;
}

/****************************************************************************
  Gives a player immediate free tech. Applies freecost
****************************************************************************/
Tech_type_id give_immediate_free_tech(struct player* pplayer)
{
  Tech_type_id tech;
  if (get_player_research(pplayer)->researching == A_UNSET) {
    return give_random_free_tech(pplayer);
  }
  tech = get_player_research(pplayer)->researching;
  do_free_cost(pplayer, tech);
  found_new_tech(pplayer, tech, FALSE, TRUE);
  return tech;
}
